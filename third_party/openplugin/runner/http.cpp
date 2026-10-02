#include "http.hpp"

#include "openplugin/manifest.hpp"

#include <algorithm>
#include <cctype>

namespace opl::runner {

// ------------------------------------------------------------------------ URLs

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool isUrlChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Printable ASCII except space, quotes, angle brackets, backslash, ^, `, {, |, }.
    if (u <= 0x20 || u >= 0x7F) return false;
    return std::string_view("\"<>\\^`{|}").find(c) == std::string_view::npos;
}

bool isHostChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

}  // namespace

std::string Url::str() const {
    const bool defaultPort = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return scheme + "://" + host + (defaultPort ? "" : ":" + std::to_string(port)) + target;
}

std::optional<Url> parseUrl(std::string_view text) {
    if (text.size() > 8192) return std::nullopt;
    if (const std::size_t hash = text.find('#'); hash != std::string_view::npos) text = text.substr(0, hash);
    if (!std::all_of(text.begin(), text.end(), isUrlChar)) return std::nullopt;
    const std::size_t colon = text.find("://");
    if (colon == std::string_view::npos) return std::nullopt;
    Url u;
    u.scheme = lower(text.substr(0, colon));
    if (u.scheme != "https" && u.scheme != "http") return std::nullopt;
    u.port = u.scheme == "https" ? 443 : 80;
    std::string_view rest = text.substr(colon + 3);
    const std::size_t pathStart = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, pathStart);
    std::string_view target = pathStart == std::string_view::npos ? std::string_view() : rest.substr(pathStart);
    if (authority.find('@') != std::string_view::npos) return std::nullopt;  // no user:password@
    if (authority.find('[') != std::string_view::npos) return std::nullopt;  // no IPv6 literals
    if (const std::size_t pc = authority.rfind(':'); pc != std::string_view::npos) {
        const std::string_view port = authority.substr(pc + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        u.port = std::stoi(std::string(port));
        if (u.port < 1 || u.port > 65535) return std::nullopt;
        authority = authority.substr(0, pc);
    }
    u.host = lower(authority);
    if (!u.host.empty() && u.host.back() == '.') u.host.pop_back();  // "example.com." is example.com
    if (u.host.empty() || u.host.size() > 253 || !std::all_of(u.host.begin(), u.host.end(), isHostChar)) return std::nullopt;
    if (u.host.front() == '.' || u.host.find("..") != std::string::npos) return std::nullopt;
    u.target = target.empty() ? "/" : std::string(target);
    if (u.target[0] == '?') u.target.insert(0, "/");
    return u;
}

std::optional<Url> resolveLocation(const Url& base, std::string_view location) {
    if (location.find("://") != std::string_view::npos) return parseUrl(location);
    if (location.substr(0, 2) == "//") return parseUrl(base.scheme + ":" + std::string(location));
    if (location.empty() || !std::all_of(location.begin(), location.end(), isUrlChar)) return std::nullopt;
    Url u = base;
    if (location[0] == '/') {
        u.target = std::string(location);
    } else if (location[0] == '?') {
        u.target = base.target.substr(0, base.target.find('?')) + std::string(location);
    } else {
        // Relative path: replace the last segment. (Dot segments are left for the server.)
        const std::string path = base.target.substr(0, base.target.find('?'));
        u.target = path.substr(0, path.rfind('/') + 1) + std::string(location);
    }
    if (const std::size_t hash = u.target.find('#'); hash != std::string::npos) u.target.resize(hash);
    return u;
}

// ---------------------------------------------------------------------- client

namespace {

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

void checkHeaders(const Headers& headers) {
    static const char* const forbidden[] = {"host",       "content-length", "transfer-encoding", "connection",
                                            "keep-alive", "upgrade",        "te",                "trailer",
                                            "expect",     "proxy-authorization", "proxy-connection"};
    if (headers.size() > 100) throw HttpError("too many request headers");
    for (const auto& [name, value] : headers) {
        if (name.empty() || name.size() > 256 || !std::all_of(name.begin(), name.end(), isTokenChar))
            throw HttpError("invalid header name '" + name + "'");
        const std::string n = lower(name);
        for (const char* f : forbidden)
            if (n == f) throw HttpError("the " + name + " header is set by the runner and can't be changed");
        if (value.size() > 16384) throw HttpError("the " + name + " header is too long");
        for (char c : value) {
            const unsigned char u = static_cast<unsigned char>(c);
            if ((u < 0x20 && c != '\t') || u == 0x7F) throw HttpError("the " + name + " header contains a control character");
        }
    }
}

bool isCredentialHeader(const std::string& name) {
    const std::string n = lower(name);
    return n == "authorization" || n == "cookie";
}

}  // namespace

HttpClient::HttpClient(std::vector<std::string> allowedHosts, Transport& transport, HttpLimits limits)
    : allowed_(std::move(allowedHosts)), transport_(transport), limits_(limits) {}

void HttpClient::checkAllowed(const Url& url) const {
    const std::string where = url.host + (url.port == 443 ? "" : ":" + std::to_string(url.port));
    bool matched = false;
    for (const std::string& pattern : allowed_) {
        if (hostMatches(pattern, url.host, url.port)) {
            matched = true;
            break;
        }
    }
    if (!matched) {
        if (allowed_.empty()) throw HttpError("this plugin has no approved network hosts, so it can't contact " + where);
        throw HttpError(where + " isn't one of this plugin's approved network hosts");
    }
    if (url.scheme == "http" && url.host != "localhost" && url.host != "127.0.0.1")
        throw HttpError("plain http:// is only allowed for localhost; use https://" + url.host);
}

HttpResponse HttpClient::request(const HttpRequest& req, const std::function<bool()>& cancelled) {
    static const char* const methods[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"};
    std::string method = req.method;
    for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (std::find_if(std::begin(methods), std::end(methods), [&](const char* m) { return method == m; }) == std::end(methods))
        throw HttpError("unsupported method '" + req.method + "'");
    if (req.body.size() > limits_.maxRequestBytes) throw HttpError("the request body is larger than 16 MiB");
    if (req.timeoutMs < 1 || req.timeoutMs > limits_.maxTimeoutMs) throw HttpError("timeout must be between 1 and 300 seconds");
    checkHeaders(req.headers);

    auto url = parseUrl(req.url);
    if (!url) throw HttpError("invalid URL '" + req.url + "' (it must be a full https:// address)");
    Headers headers = req.headers;
    std::string body = req.body;

    for (int hop = 0;; ++hop) {
        checkAllowed(*url);
        if (cancelled && cancelled()) throw HttpError("cancelled");
        HttpResponse resp = transport_.send(*url, method, headers, body, req.timeoutMs, limits_.maxResponseBytes, cancelled);
        resp.url = url->str();
        const bool redirect = resp.status == 301 || resp.status == 302 || resp.status == 303 || resp.status == 307 || resp.status == 308;
        if (!redirect) return resp;

        std::string location;
        for (const auto& [name, value] : resp.headers)
            if (name == "location") location = value;
        if (location.empty()) return resp;  // a 3xx without a destination is just a response
        if (hop + 1 > limits_.maxRedirects) throw HttpError("too many redirects");
        auto next = resolveLocation(*url, location);
        if (!next) throw HttpError("the server redirected to an invalid address");
        if (url->scheme == "https" && next->scheme == "http") throw HttpError("refused a redirect from https:// to http://");
        try {
            checkAllowed(*next);
        } catch (const HttpError& e) {
            throw HttpError(std::string("refused a redirect: ") + e.what());
        }
        if (!next->sameOrigin(*url))
            headers.erase(std::remove_if(headers.begin(), headers.end(), [](const auto& h) { return isCredentialHeader(h.first); }),
                          headers.end());
        // Browsers' rules: 303 always becomes GET; 301/302 turn POST into GET; 307/308 keep everything.
        if ((resp.status == 303 && method != "HEAD") || ((resp.status == 301 || resp.status == 302) && method == "POST")) {
            method = "GET";
            body.clear();
            headers.erase(std::remove_if(headers.begin(), headers.end(),
                                         [](const auto& h) { return lower(h.first) == "content-type"; }),
                          headers.end());
        }
        url = std::move(next);
    }
}

}  // namespace opl::runner
