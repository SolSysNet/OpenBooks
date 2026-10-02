// Transport on Linux and macOS: the system libcurl (preinstalled on macOS; libcurl4-openssl-dev or
// libcurl-devel on Linux, needed only to build the runner). TLS is verified against the system
// trust store; https_proxy/no_proxy apply.

#include "http.hpp"

#include <curl/curl.h>

#include <mutex>

namespace opl::runner {

namespace {

struct Transfer {
    HttpResponse* resp;
    std::size_t maxBytes;
    bool tooLarge = false;
    const std::function<bool()>* cancelled;
};

size_t onBody(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (t->resp->body.size() + n > t->maxBytes) {
        t->tooLarge = true;
        return 0;  // aborts the transfer
    }
    t->resp->body.append(data, n);
    return n;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = size * count;
    std::string line(data, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {
        t->resp->headers.clear();  // a new response (e.g. after "100 Continue")
        return n;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return n;
    std::string name = line.substr(0, colon);
    for (char& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    t->resp->headers.emplace_back(std::move(name), std::move(value));
    return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(user);
    return (*t->cancelled && (*t->cancelled)()) ? 1 : 0;
}

class CurlTransport : public Transport {
public:
    CurlTransport() {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    HttpResponse send(const Url& url, const std::string& method, const Headers& headers, const std::string& body,
                      int timeoutMs, std::size_t maxResponseBytes, const std::function<bool()>& cancelled) override {
        CURL* curl = curl_easy_init();
        if (!curl) throw HttpError("cannot start an HTTP request");
        struct Cleanup {
            CURL* c;
            curl_slist* h = nullptr;
            ~Cleanup() {
                curl_slist_free_all(h);
                curl_easy_cleanup(c);
            }
        } cleanup{curl};

        HttpResponse resp;
        Transfer t{&resp, maxResponseBytes, false, &cancelled};
        const std::string target = url.str();
        curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // the client checks every hop
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED);  // never pick up the user's ~/.netrc passwords
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs));
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "openplugin-runner/1");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        if (method == "HEAD") {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        } else if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        cleanup.h = curl_slist_append(cleanup.h, "Expect:");  // no "100-continue" round trip
        for (const auto& [name, value] : headers)
            cleanup.h = curl_slist_append(cleanup.h, (name + ": " + value).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, cleanup.h);

        const CURLcode rc = curl_easy_perform(curl);
        if (t.tooLarge) throw HttpError(url.host + ": the response is larger than 16 MiB");
        if (rc == CURLE_ABORTED_BY_CALLBACK) throw HttpError("cancelled");
        if (rc != CURLE_OK) {
            std::string why;
            switch (rc) {
                case CURLE_OPERATION_TIMEDOUT: why = "the request timed out"; break;
                case CURLE_COULDNT_RESOLVE_HOST: why = "couldn't find the server"; break;
                case CURLE_COULDNT_CONNECT: why = "couldn't connect to the server"; break;
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_SSL_CACERT_BADFILE:
                case CURLE_SSL_CONNECT_ERROR: why = "the server's certificate isn't trusted"; break;
                default: why = curl_easy_strerror(rc);
            }
            throw HttpError(url.host + ": " + why);
        }
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.status = static_cast<int>(status);
        return resp;
    }
};

}  // namespace

std::unique_ptr<Transport> makeSystemTransport() { return std::make_unique<CurlTransport>(); }

}  // namespace opl::runner
