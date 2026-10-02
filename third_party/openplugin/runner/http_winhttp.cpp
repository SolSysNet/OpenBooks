// Transport on Windows: WinHTTP, part of Windows (like CNG for the apps' encryption). TLS is verified
// against the Windows certificate store and the system proxy settings apply.

#include "http.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace opl::runner {

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

[[noreturn]] void fail(const Url& url) {
    const DWORD code = GetLastError();
    std::string why;
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: why = "the request timed out"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: why = "couldn't find the server"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: why = "couldn't connect to the server"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: why = "the connection was reset"; break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED: why = "the server's certificate isn't trusted"; break;
        case ERROR_WINHTTP_OPERATION_CANCELLED: why = "cancelled"; break;
        default: {
            wchar_t* buf = nullptr;
            const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                               FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                           GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
            why = n ? narrow(std::wstring_view(buf, n)) : "error " + std::to_string(code);
            if (buf) LocalFree(buf);
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r' || why.back() == '.')) why.pop_back();
        }
    }
    throw HttpError(url.host + ": " + why);
}

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET handle) : h(handle) {}
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
};

void setDword(HINTERNET h, DWORD option, DWORD value) { WinHttpSetOption(h, option, &value, sizeof value); }

class WinHttpTransport : public Transport {
public:
    HttpResponse send(const Url& url, const std::string& method, const Headers& headers, const std::string& body,
                      int timeoutMs, std::size_t maxResponseBytes, const std::function<bool()>& cancelled) override {
        // Automatic proxy (Windows 8.1+) follows the system settings; fall back for older systems.
        HInternet session(WinHttpOpen(L"openplugin-runner/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h)
            session.h = WinHttpOpen(L"openplugin-runner/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) fail(url);
        WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows versions without TLS 1.3 support
            WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }

        HInternet connect(WinHttpConnect(session.h, widen(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0));
        if (!connect.h) fail(url);
        HInternet request(WinHttpOpenRequest(connect.h, widen(method).c_str(), widen(url.target).c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             url.scheme == "https" ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) fail(url);
        // Redirects are the client's job (each hop is checked); no cookies; never send the user's
        // Windows credentials or answer authentication challenges on the plugin's behalf.
        setDword(request.h, WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);
        setDword(request.h, WINHTTP_OPTION_DISABLE_FEATURE, WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION);
        setDword(request.h, WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH);

        std::wstring headerText;
        for (const auto& [name, value] : headers) headerText += widen(name) + L": " + widen(value) + L"\r\n";
        if (!WinHttpSendRequest(request.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                headerText.empty() ? 0 : static_cast<DWORD>(-1L),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0))
            fail(url);
        if (!WinHttpReceiveResponse(request.h, nullptr)) fail(url);

        HttpResponse resp;
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &size, WINHTTP_NO_HEADER_INDEX))
            fail(url);
        resp.status = static_cast<int>(status);

        size = 0;
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (size > 0 && size <= 256 * 1024) {
            std::vector<wchar_t> raw(size / sizeof(wchar_t) + 1);
            if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                const std::string text = narrow(std::wstring_view(raw.data(), size / sizeof(wchar_t)));
                std::size_t pos = text.find("\r\n");  // skip the status line
                while (pos != std::string::npos && pos + 2 < text.size()) {
                    const std::size_t end = text.find("\r\n", pos + 2);
                    const std::string line = text.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2);
                    pos = end;
                    const std::size_t colon = line.find(':');
                    if (colon == std::string::npos || colon == 0) continue;
                    std::string name = line.substr(0, colon);
                    for (char& c : name)
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    std::string value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
                    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
                    resp.headers.emplace_back(std::move(name), std::move(value));
                }
            }
        }

        for (;;) {
            if (cancelled && cancelled()) throw HttpError("cancelled");
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available)) fail(url);
            if (available == 0) break;
            if (resp.body.size() + available > maxResponseBytes) throw HttpError(url.host + ": the response is larger than 16 MiB");
            const std::size_t old = resp.body.size();
            resp.body.resize(old + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, resp.body.data() + old, available, &read)) fail(url);
            resp.body.resize(old + read);
        }
        return resp;
    }
};

}  // namespace

std::unique_ptr<Transport> makeSystemTransport() { return std::make_unique<WinHttpTransport>(); }

}  // namespace opl::runner
