#pragma once

// HTTPS for plugins (docs/design.md, section 7.3).
//
// HttpClient holds every rule; a Transport only performs one request with no redirects. That keeps
// the policy identical on every platform and testable without a network:
//
//   - Only hosts matching an approved pattern (manifest "network", narrowed by the user).
//   - HTTPS only. Plain HTTP is allowed solely for "localhost" and "127.0.0.1" with a port the user
//     approved explicitly (local mock servers while developing a plugin).
//   - Redirects are followed by the client, at most 10, and every hop is checked against the same
//     rules. Leaving a host drops Authorization and Cookie headers. HTTPS never redirects to HTTP.
//   - Methods, header names and values are validated; headers that control the connection itself
//     (Host, Content-Length, ...) can't be set.
//   - Request and response bodies are limited to 16 MiB; timeouts to 1-300 seconds.

#include "openplugin/error.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opl::runner {

class HttpError : public Error {
public:
    using Error::Error;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Url {
    std::string scheme;  // "https" or "http"
    std::string host;    // lowercase; a DNS name or an IPv4 address
    int port = 443;
    std::string target;  // path and query, starting with '/'

    std::string str() const;
    bool sameOrigin(const Url& o) const { return scheme == o.scheme && host == o.host && port == o.port; }
};

// Absolute http(s) URLs only. No user info, no IPv6 literals; the fragment is dropped.
std::optional<Url> parseUrl(std::string_view text);
// A redirect's Location relative to the URL that sent it.
std::optional<Url> resolveLocation(const Url& base, std::string_view location);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::string body;
    int timeoutMs = 30000;
};

struct HttpResponse {
    int status = 0;
    Headers headers;  // names lowercased, in order received
    std::string body;
    std::string url;  // the final URL, after redirects
};

struct HttpLimits {
    std::size_t maxRequestBytes = 16u << 20;
    std::size_t maxResponseBytes = 16u << 20;
    int maxRedirects = 10;
    int maxTimeoutMs = 300000;
};

// One request, no redirect following, no cookies, no automatic authentication, TLS verified
// against the OS trust store. Throws HttpError for anything that isn't an HTTP response.
class Transport {
public:
    virtual ~Transport() = default;
    virtual HttpResponse send(const Url& url, const std::string& method, const Headers& headers, const std::string& body,
                              int timeoutMs, std::size_t maxResponseBytes, const std::function<bool()>& cancelled) = 0;
};

// WinHTTP on Windows; the system libcurl elsewhere.
std::unique_ptr<Transport> makeSystemTransport();

class HttpClient {
public:
    HttpClient(std::vector<std::string> allowedHosts, Transport& transport, HttpLimits limits = {});

    // Throws HttpError with a message for the plugin author ("api.example.com isn't in this
    // plugin's approved hosts", ...).
    HttpResponse request(const HttpRequest& req, const std::function<bool()>& cancelled = {});

    // Throws HttpError if this URL may not be contacted.
    void checkAllowed(const Url& url) const;

private:
    std::vector<std::string> allowed_;
    Transport& transport_;
    HttpLimits limits_;
};

}  // namespace opl::runner
