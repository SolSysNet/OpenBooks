// Tests for openplugin-runner: the codecs and HTTP policy directly, and the sandbox, the op API and
// the protocol end to end by running the real runner on small Lua plugins.

#include "codec.hpp"
#include "http.hpp"

#include "openplugin/json.hpp"
#include "openplugin/process.hpp"
#include "openplugin/registry.hpp"
#include "openplugin/rpc.hpp"

#include "test.hpp"

#include <atomic>
#include <chrono>
#include <climits>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace opl;
using namespace opl::runner;
using namespace testing;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// ===================================================================== codecs

TEST(codec_money_parse_and_format) {
    CHECK_EQ(*parseMoney("1234.50"), 123450);
    CHECK_EQ(*parseMoney("-0.05"), -5);
    CHECK_EQ(*parseMoney("12"), 1200);
    CHECK_EQ(*parseMoney("12.5"), 1250);
    CHECK_EQ(*parseMoney("0"), 0);
    for (const char* bad : {"", "-", "1.", ".5", "1.234", "1,000", "+1", "abc", "1e3", "12345678901234567", "1.2.3", "--1"})
        CHECK(!parseMoney(bad).has_value());
    CHECK_EQ(formatMoney(123450), std::string("1234.50"));
    CHECK_EQ(formatMoney(-5), std::string("-0.05"));
    CHECK_EQ(formatMoney(0), std::string("0.00"));
    CHECK_EQ(formatMoney(-100), std::string("-1.00"));
    CHECK_EQ(formatMoney(INT64_MIN), std::string("-92233720368547758.08"));
}

TEST(codec_money_arithmetic) {
    CHECK_EQ(addMoney(110, 220), 330);
    CHECK_EQ(subMoney(100, 250), -150);
    CHECK_THROWS(addMoney(INT64_MAX, 1), Error, "out of range");
    CHECK_THROWS(subMoney(INT64_MIN, 1), Error, "out of range");
    // Half away from zero, like the apps.
    CHECK_EQ(mulMoney(1000, "0.0725"), 73);    // 72.5
    CHECK_EQ(mulMoney(-1000, "0.0725"), -73);
    CHECK_EQ(mulMoney(1000, "-0.0725"), -73);
    CHECK_EQ(mulMoney(-1000, "-0.0725"), 73);
    CHECK_EQ(mulMoney(12345, "1.5"), 18518);   // 18517.5
    CHECK_EQ(mulMoney(1, "0.5"), 1);
    CHECK_EQ(mulMoney(1, "0.499999"), 0);
    CHECK_EQ(mulMoney(100, "3"), 300);
    CHECK_EQ(mulMoney(0, "123.45"), 0);
    CHECK_EQ(mulMoney(INT64_MAX, "1"), INT64_MAX);
    CHECK_EQ(mulMoney(INT64_MAX, "0.5"), INT64_MAX / 2 + 1);  // ...807 / 2 = ...403.5 -> ...404
    CHECK_EQ(mulMoney(900000000000000000LL, "10"), 9000000000000000000LL);
    CHECK_THROWS(mulMoney(INT64_MAX / 2, "3"), Error, "out of range");
    CHECK_THROWS(mulMoney(100, "1.2345678"), Error, "invalid factor");
    CHECK_THROWS(mulMoney(100, "abc"), Error, "invalid factor");
    CHECK_THROWS(mulMoney(100, "1."), Error, "invalid factor");
}

TEST(codec_base64_rfc4648) {
    const std::pair<const char*, const char*> vectors[] = {{"", ""},         {"f", "Zg=="},       {"fo", "Zm8="},
                                                           {"foo", "Zm9v"},  {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
                                                           {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, encoded] : vectors) {
        CHECK_EQ(base64Encode(plain), std::string(encoded));
        CHECK_EQ(*base64Decode(encoded), std::string(plain));
    }
    const std::string bytes("\x00\xff\x10\x80", 4);
    CHECK_EQ(*base64Decode(base64Encode(bytes)), bytes);
    for (const char* bad : {"Zg=", "Zm9v!", "Zg==Zg==", "=Zg=", "Z===", "Zm9"}) CHECK(!base64Decode(bad).has_value());
}

TEST(codec_hex) {
    CHECK_EQ(hexEncode(std::string("\x00\xff\x10", 3)), std::string("00ff10"));
    CHECK_EQ(*hexDecode("00FFab"), std::string("\x00\xff\xab", 3));
    CHECK(!hexDecode("0g").has_value());
    CHECK(!hexDecode("abc").has_value());
}

TEST(codec_hmac_sha256_rfc4231) {
    CHECK_EQ(hexEncode(hmacSha256Raw(std::string(20, '\x0b'), "Hi There")),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK_EQ(hexEncode(hmacSha256Raw("Jefe", "what do ya want for nothing?")),
             std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK_EQ(hexEncode(hmacSha256Raw(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST(codec_random_bytes) {
    const std::string a = randomBytes(32), b = randomBytes(32);
    CHECK_EQ(a.size(), std::size_t(32));
    CHECK(a != b);
    CHECK(randomBytes(0).empty());
}

// ================================================================ HTTP policy

struct FakeTransport : Transport {
    struct Call {
        std::string method, url, body;
        Headers headers;
        bool has(const std::string& name) const {
            for (const auto& h : headers)
                if (h.first == name) return true;
            return false;
        }
    };
    std::vector<Call> calls;
    std::function<HttpResponse(const Url&, const std::string&)> handler = [](const Url&, const std::string&) {
        HttpResponse r;
        r.status = 200;
        r.body = "ok";
        return r;
    };
    HttpResponse send(const Url& url, const std::string& method, const Headers& headers, const std::string& body, int,
                      std::size_t, const std::function<bool()>&) override {
        calls.push_back({method, url.str(), body, headers});
        return handler(url, method);
    }
};

HttpResponse redirectTo(int status, const std::string& location) {
    HttpResponse r;
    r.status = status;
    r.headers.emplace_back("location", location);
    return r;
}

HttpRequest get(const std::string& url) {
    HttpRequest r;
    r.url = url;
    return r;
}

TEST(http_parses_urls) {
    auto u = parseUrl("https://API.Example.com/v1/items?x=1#frag");
    CHECK(u.has_value());
    CHECK_EQ(u->host, std::string("api.example.com"));
    CHECK_EQ(u->port, 443);
    CHECK_EQ(u->target, std::string("/v1/items?x=1"));
    CHECK_EQ(u->str(), std::string("https://api.example.com/v1/items?x=1"));
    CHECK_EQ(parseUrl("https://h.example.com:8443")->port, 8443);
    CHECK_EQ(parseUrl("https://h.example.com:8443")->target, std::string("/"));
    CHECK_EQ(parseUrl("http://localhost:8080/x")->str(), std::string("http://localhost:8080/x"));
    CHECK_EQ(parseUrl("https://h.example.com?q=1")->target, std::string("/?q=1"));
    CHECK_EQ(parseUrl("https://example.com./")->host, std::string("example.com"));
    for (const char* bad : {"https://user:pw@h.com/", "https://[::1]/", "ftp://h.com/", "https://h.com:0/", "https://h.com:99999/",
                            "https://h com/", "https://h.com/a b", "//h.com/", "https://h_x.com/", "h.com", "https://",
                            "https://h.com:/", "https://..com/", "https://h.com/\xC3\xA9"})
        CHECK(!parseUrl(bad).has_value());
}

TEST(http_resolves_redirect_locations) {
    const Url base = *parseUrl("https://a.example.com/x/y?q=1");
    CHECK_EQ(resolveLocation(base, "/z")->str(), std::string("https://a.example.com/z"));
    CHECK_EQ(resolveLocation(base, "z")->str(), std::string("https://a.example.com/x/z"));
    CHECK_EQ(resolveLocation(base, "?p=2")->str(), std::string("https://a.example.com/x/y?p=2"));
    CHECK_EQ(resolveLocation(base, "https://b.example.com/")->str(), std::string("https://b.example.com/"));
    CHECK_EQ(resolveLocation(base, "//b.example.com/p")->str(), std::string("https://b.example.com/p"));
    CHECK(!resolveLocation(base, "").has_value());
    CHECK(!resolveLocation(base, "/a b").has_value());
}

TEST(http_allows_only_approved_hosts) {
    FakeTransport t;
    HttpClient client({"api.example.com"}, t);
    CHECK_EQ(client.request(get("https://api.example.com/v1")).body, std::string("ok"));
    CHECK_THROWS(client.request(get("https://evil.example.org/")), HttpError, "isn't one of this plugin's approved network hosts");
    CHECK_THROWS(client.request(get("https://api.example.com:8443/")), HttpError, "api.example.com:8443 isn't one of");
    CHECK_THROWS(client.request(get("http://api.example.com/")), HttpError, "isn't one of");
    CHECK_THROWS(client.request(get("https://API.EXAMPLE.COM.evil.org/")), HttpError, "isn't one of");
    CHECK_EQ(t.calls.size(), std::size_t(1));  // refused requests never reach the network

    HttpClient local({"localhost:8080"}, t);
    CHECK_EQ(local.request(get("http://localhost:8080/")).status, 200);
    HttpClient plainRemote({"api.example.com:8080"}, t);
    CHECK_THROWS(plainRemote.request(get("http://api.example.com:8080/")), HttpError, "only allowed for localhost");
    HttpClient none({}, t);
    CHECK_THROWS(none.request(get("https://api.example.com/")), HttpError, "has no approved network hosts");
}

TEST(http_checks_every_redirect_hop) {
    FakeTransport t;
    t.handler = [](const Url& u, const std::string&) {
        if (u.host == "api.example.com") return redirectTo(302, "https://cdn.example.com/file");
        HttpResponse r;
        r.status = 200;
        r.body = "file";
        return r;
    };
    HttpClient client({"api.example.com", "cdn.example.com"}, t);
    HttpRequest req = get("https://api.example.com/download");
    req.headers = {{"Authorization", "Bearer secret"}, {"Cookie", "s=1"}, {"Accept", "*/*"}};
    const HttpResponse r = client.request(req);
    CHECK_EQ(r.body, std::string("file"));
    CHECK_EQ(r.url, std::string("https://cdn.example.com/file"));
    CHECK_EQ(t.calls.size(), std::size_t(2));
    CHECK(t.calls[0].has("Authorization"));
    CHECK(!t.calls[1].has("Authorization"));  // credentials never follow a redirect to another host
    CHECK(!t.calls[1].has("Cookie"));
    CHECK(t.calls[1].has("Accept"));

    // Same host: credentials stay.
    FakeTransport same;
    same.handler = [](const Url& u, const std::string&) {
        if (u.target == "/old") return redirectTo(301, "/new");
        HttpResponse r;
        r.status = 200;
        return r;
    };
    HttpClient c2({"api.example.com"}, same);
    HttpRequest req2 = get("https://api.example.com/old");
    req2.headers = {{"Authorization", "Bearer secret"}};
    c2.request(req2);
    CHECK(same.calls[1].has("Authorization"));
}

TEST(http_refuses_redirects_to_unapproved_places) {
    FakeTransport t;
    t.handler = [](const Url&, const std::string&) { return redirectTo(302, "https://evil.example.org/steal"); };
    HttpClient client({"api.example.com"}, t);
    CHECK_THROWS(client.request(get("https://api.example.com/")), HttpError, "refused a redirect");
    CHECK_EQ(t.calls.size(), std::size_t(1));

    FakeTransport down;
    down.handler = [](const Url&, const std::string&) { return redirectTo(302, "http://localhost:8080/"); };
    HttpClient c2({"api.example.com", "localhost:8080"}, down);
    CHECK_THROWS(c2.request(get("https://api.example.com/")), HttpError, "https:// to http://");

    FakeTransport loop;
    loop.handler = [](const Url&, const std::string&) { return redirectTo(302, "/again"); };
    HttpClient c3({"api.example.com"}, loop);
    CHECK_THROWS(c3.request(get("https://api.example.com/")), HttpError, "too many redirects");
    CHECK_EQ(loop.calls.size(), std::size_t(11));
}

TEST(http_redirect_methods_follow_browser_rules) {
    for (int status : {301, 302, 303, 307, 308}) {
        FakeTransport t;
        t.handler = [&](const Url& u, const std::string&) {
            if (u.target == "/submit") return redirectTo(status, "/done");
            HttpResponse r;
            r.status = 200;
            return r;
        };
        HttpClient client({"api.example.com"}, t);
        HttpRequest req = get("https://api.example.com/submit");
        req.method = "POST";
        req.body = "{\"a\":1}";
        req.headers = {{"Content-Type", "application/json"}};
        client.request(req);
        const bool keeps = status == 307 || status == 308;
        CHECK_EQ(t.calls[1].method, std::string(keeps ? "POST" : "GET"));
        CHECK_EQ(t.calls[1].body.empty(), !keeps);
        CHECK_EQ(t.calls[1].has("Content-Type"), keeps);
    }
    // A 3xx with no Location is just a response.
    FakeTransport t;
    t.handler = [](const Url&, const std::string&) {
        HttpResponse r;
        r.status = 304;
        return r;
    };
    HttpClient client({"api.example.com"}, t);
    CHECK_EQ(client.request(get("https://api.example.com/")).status, 304);
}

TEST(http_validates_requests) {
    FakeTransport t;
    HttpLimits limits;
    limits.maxRequestBytes = 10;
    HttpClient client({"api.example.com"}, t, limits);
    HttpRequest req = get("https://api.example.com/");
    req.headers = {{"Host", "evil.example.org"}};
    CHECK_THROWS(client.request(req), HttpError, "set by the runner");
    req.headers = {{"content-length", "5"}};
    CHECK_THROWS(client.request(req), HttpError, "set by the runner");
    req.headers = {{"Bad Header", "x"}};
    CHECK_THROWS(client.request(req), HttpError, "invalid header name");
    req.headers = {{"X-Inject", "a\r\nHost: evil"}};
    CHECK_THROWS(client.request(req), HttpError, "control character");
    req.headers = {};
    req.method = "TRACE";
    CHECK_THROWS(client.request(req), HttpError, "unsupported method");
    req.method = "post";  // case-insensitive
    req.body = std::string(11, 'x');
    CHECK_THROWS(client.request(req), HttpError, "larger than");
    req.body = "ok";
    req.timeoutMs = 0;
    CHECK_THROWS(client.request(req), HttpError, "timeout");
    req.timeoutMs = 1000;
    client.request(req);
    CHECK_EQ(t.calls.back().method, std::string("POST"));
    CHECK_THROWS(client.request(get("not a url")), HttpError, "invalid URL");
}

// ============================================================ local HTTP server

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void closeSocket(Socket s) { ::close(s); }
#endif

// A tiny HTTP/1.1 server on 127.0.0.1 for exercising the real transport. One connection at a time,
// "Connection: close" on every response.
class TestServer {
public:
    TestServer() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(listener_, 8);
        socklen_t len = sizeof addr;
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~TestServer() {
        stop_ = true;
#ifdef _WIN32
        closeSocket(listener_);
#else
        ::shutdown(listener_, SHUT_RDWR);
        closeSocket(listener_);
#endif
        thread_.join();
    }
    int port() const { return port_; }

    struct Seen {
        std::string method, target, body;
        std::map<std::string, std::string> headers;
    };
    std::vector<Seen> seen() {
        std::lock_guard<std::mutex> lock(m_);
        return seen_;
    }

private:
    void loop() {
        while (!stop_) {
            const Socket c = ::accept(listener_, nullptr, nullptr);
            if (c == kNoSocket) return;
            handle(c);
            closeSocket(c);
        }
    }

    void handle(Socket c) {
        std::string data;
        char buf[16384];
        std::size_t headerEnd;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            const int n = static_cast<int>(::recv(c, buf, sizeof buf, 0));
            if (n <= 0) return;
            data.append(buf, static_cast<std::size_t>(n));
        }
        Seen s;
        std::size_t lineEnd = data.find("\r\n");
        const std::string requestLine = data.substr(0, lineEnd);
        s.method = requestLine.substr(0, requestLine.find(' '));
        s.target = requestLine.substr(s.method.size() + 1, requestLine.rfind(' ') - s.method.size() - 1);
        std::size_t pos = lineEnd + 2;
        while (pos < headerEnd) {
            const std::size_t end = data.find("\r\n", pos);
            const std::string line = data.substr(pos, end - pos);
            pos = end + 2;
            const std::size_t colon = line.find(':');
            std::string name = line.substr(0, colon);
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            s.headers[name] = line.substr(line.find_first_not_of(' ', colon + 1));
        }
        const std::size_t length = s.headers.count("content-length") ? std::stoul(s.headers["content-length"]) : 0;
        s.body = data.substr(headerEnd + 4);
        while (s.body.size() < length) {
            const int n = static_cast<int>(::recv(c, buf, sizeof buf, 0));
            if (n <= 0) return;
            s.body.append(buf, static_cast<std::size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            seen_.push_back(s);
        }

        std::string status = "200 OK", extra, body;
        if (s.target == "/hello") {
            body = "hi";
            extra = "X-Test: yes\r\nX-Multi: a\r\nX-Multi: b\r\n";
        } else if (s.target == "/echo") {
            body = s.method + "|" + (s.headers.count("authorization") ? s.headers["authorization"] : "-") + "|" + s.body;
        } else if (s.target == "/redirect-same") {
            status = "302 Found";
            extra = "Location: /hello\r\n";
        } else if (s.target == "/redirect-303") {
            status = "303 See Other";
            extra = "Location: /echo\r\n";
        } else if (s.target == "/redirect-away") {
            status = "302 Found";
            extra = "Location: http://127.0.0.1:1/steal\r\n";
        } else if (s.target == "/big") {
            body.assign((16u << 20) + 100, 'x');
        } else {
            status = "404 Not Found";
            body = "no";
        }
        const std::string response = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                                     "\r\nConnection: close\r\n" + extra + "\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            const int n = static_cast<int>(::send(c, response.data() + sent, static_cast<int>(response.size() - sent), 0));
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    Socket listener_ = kNoSocket;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex m_;
    std::vector<Seen> seen_;
};

// ================================================================ runner harness

const char* kEvalMain = R"LUA(
op.command("eval", function(ctx)
  local f, err = load(ctx.code, "=eval")
  if not f then error(err, 0) end
  return f()
end)
op.extension("test.ext", { echo = function(p) return p end })
)LUA";

// A fake app: runs the real runner on a throwaway plugin and answers its requests.
struct Host {
    TempDir tmp;
    fs::path dir;
    std::unique_ptr<rpc::Channel> ch;
    std::map<std::string, std::pair<std::string, bool>> store;  // key -> value, secret
    std::vector<std::string> logs;                               // "level: message"
    std::vector<std::pair<std::string, json::Value>> calls;      // what the plugin asked the app
    std::vector<json::Value> progress;
    // Sees each host call first; returns true if it took the call (and will answer it, now or later).
    std::function<bool(const std::string&, const json::Value&, rpc::Responder)> custom;

    explicit Host(const std::string& mainLua = kEvalMain, json::Object manifestChanges = {},
                  std::map<std::string, std::string> extraFiles = {}) {
        dir = tmp.path / "lab";
        json::Object manifest{{"schema", 1},
                              {"id", "org.example.lab"},
                              {"name", "Lab"},
                              {"version", "1.0.0"},
                              {"publisher", "Tests"},
                              {"apps", json::Object{{"openbooks", ">=0.1"}}},
                              {"protocol", 1},
                              {"runtime", "lua"},
                              {"main", "main.lua"},
                              {"permissions", json::Array{json::Value("read:invoices"), json::Value("store"),
                                                          json::Value("propose:payments")}},
                              {"extensions", json::Array{json::Value("test.ext")}}};
        for (const auto& [k, v] : manifestChanges) manifest.set(k, v);
        writeFile(dir / "plugin.json", json::write(json::Value(manifest)));
        writeFile(dir / "main.lua", mainLua);
        for (const auto& [path, content] : extraFiles) writeFile(dir / fs::u8path(path), content);

        SpawnOptions o;
        o.argv = {fs::absolute(OPENPLUGIN_RUNNER).u8string(), "--plugin", dir.u8string()};
        o.workingDir = dir;
        o.environment = scrubbedEnvironment();
        ch = std::make_unique<rpc::Channel>(Process::spawn(o));
        ch->onRequest([this](const std::string& method, const json::Value& params, rpc::Responder respond) {
            calls.emplace_back(method, params);
            if (custom && custom(method, params, respond)) return;
            if (method == "host/read") {
                if (params.find("view")->asString() == "ledger") throw rpc::RemoteError(rpc::PermissionDenied, "read:ledger wasn't granted");
                respond.result(json::Object{{"rows", json::Array{json::Object{{"id", 1}, {"total", "10.00"}}}},
                                            {"query", *params.find("query")}});
            } else if (method == "store/get") {
                auto it = store.find(params.find("key")->asString());
                respond.result(it == store.end() ? json::Value() : json::Value(it->second.first));
            } else if (method == "store/set") {
                store[params.find("key")->asString()] = {params.find("value")->asString(), params.find("secret")->asBool()};
                respond.result(nullptr);
            } else if (method == "store/delete") {
                store.erase(params.find("key")->asString());
                respond.result(nullptr);
            } else if (method == "host/propose") {
                respond.result(json::Object{{"applied", true}, {"ids", json::Array{json::Value(5)}}});
            } else if (method == "ui/show" || method == "ui/update" || method == "ui/close") {
                respond.result(nullptr);
            } else {
                throw rpc::RemoteError(rpc::MethodNotFound, "unknown method " + method);
            }
        });
        ch->onNotification([this](const std::string& method, const json::Value& params) {
            if (method == "$/log" || method == "ui/notify")
                logs.push_back(params.find("level")->asString() + ": " + params.find("message")->asString());
            else if (method == "$/progress")
                progress.push_back(params);
        });
    }

    json::Object initParams(int port = 0) const {
        json::Array network;
        if (port) network.emplace_back("127.0.0.1:" + std::to_string(port));
        return json::Object{{"protocol", 1},
                            {"app", json::Object{{"id", "openbooks"}, {"version", "0.6.0"}}},
                            {"granted", json::Array{json::Value("read:invoices"), json::Value("store")}},
                            {"network", json::Value(std::move(network))},
                            {"locale", "en-US"},
                            {"hash", hashPluginFolder(dir)},
                            {"limits", json::Object{{"memoryBytes", 64 << 20}}}};
    }

    rpc::Outcome call(const std::string& method, json::Value params, std::chrono::milliseconds timeout = 20s) {
        std::optional<rpc::Outcome> result;
        ch->request(method, std::move(params), timeout, [&](const rpc::Outcome& o) { result = o; });
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!result && std::chrono::steady_clock::now() < deadline) ch->poll(20ms);
        if (!result) {
            rpc::Outcome o;
            o.status = rpc::Outcome::Status::Error;
            o.message = "test gave up waiting";
            return o;
        }
        return *result;
    }

    rpc::Outcome init(int port = 0) { return init(initParams(port)); }
    rpc::Outcome init(json::Object params) {
        rpc::Outcome o = call("initialize", json::Value(std::move(params)));
        if (!o.ok() && std::getenv("OPENPLUGIN_TEST_VERBOSE"))
            std::cerr << "  initialize failed (" << o.code << "): " << o.message << "\n  log: " << ch->log() << "\n";
        return o;
    }

    rpc::Outcome eval(const std::string& code, std::chrono::milliseconds timeout = 20s) {
        return call("command/run", json::Object{{"id", "eval"}, {"context", json::Object{{"code", code}}}}, timeout);
    }

    // Runs `code` and expects success; returns the result.
    json::Value ok(const std::string& code) {
        const rpc::Outcome o = eval(code);
        if (!o.ok()) {
            ++g_failures;
            std::cerr << "  eval failed: " << o.message << "\n  code: " << code << "\n  log: " << ch->log() << "\n";
        }
        return o.result;
    }

    bool logged(const std::string& fragment) const {
        for (const std::string& l : logs)
            if (l.find(fragment) != std::string::npos) return true;
        return false;
    }
};

bool contains(const std::string& haystack, const std::string& needle) { return haystack.find(needle) != std::string::npos; }

// ============================================================ runner: protocol

TEST(runner_initializes_and_lists_handlers) {
    Host h;
    const rpc::Outcome o = h.init();
    CHECK(o.ok());
    CHECK_EQ(o.result.find("name")->asString(), std::string("Lab"));
    CHECK(o.result.find("commands")->asArray() == json::Array{json::Value("eval")});
    CHECK(o.result.find("extensions")->asArray() == json::Array{json::Value("test.ext")});
    CHECK_EQ(h.ok("return 1 + 1").asInt(), 2);
    CHECK_EQ(h.ok("return op.plugin.id").asString(), std::string("org.example.lab"));
    CHECK_EQ(h.ok("return op.app.version").asString(), std::string("0.6.0"));
    CHECK_EQ(h.ok("return #op.permissions").asInt(), 2);
    CHECK(h.call("shutdown", nullptr).ok());
}

TEST(runner_refuses_files_that_changed_since_approval) {
    {
        Host h;
        json::Object params = h.initParams();
        params.set("hash", std::string(64, '0'));
        const rpc::Outcome o = h.init(params);
        CHECK_EQ(o.code, int(rpc::FilesChanged));
        CHECK(contains(o.message, "don't match what you approved"));
    }
    {
        Host h;
        const json::Object params = h.initParams();  // the hash the user approved
        writeFile(h.dir / "main.lua", std::string(kEvalMain) + "\n-- changed after approval\n");
        const rpc::Outcome o = h.init(params);
        CHECK_EQ(o.code, int(rpc::FilesChanged));
    }
}

TEST(runner_requires_initialize_first) {
    Host h;
    const rpc::Outcome o = h.eval("return 1");
    CHECK_EQ(o.code, int(rpc::InvalidRequest));
    CHECK(h.init().ok());
    CHECK_EQ(h.call("initialize", json::Value(h.initParams())).code, int(rpc::InvalidRequest));  // only once
}

TEST(runner_dispatches_extensions_and_reports_unknown_methods) {
    Host h;
    CHECK(h.init().ok());
    const rpc::Outcome echo = h.call("test.ext/echo", json::Object{{"amount", "12.50"}, {"n", 3}});
    CHECK(echo.ok());
    CHECK_EQ(echo.result.find("amount")->asString(), std::string("12.50"));
    CHECK_EQ(h.call("test.ext/nope", json::Object{}).code, int(rpc::MethodNotFound));
    CHECK_EQ(h.call("other.ext/echo", json::Object{}).code, int(rpc::MethodNotFound));
    CHECK_EQ(h.call("nonsense", json::Object{}).code, int(rpc::MethodNotFound));
    CHECK_EQ(h.call("command/run", json::Object{{"id", "missing"}}).code, int(rpc::MethodNotFound));
}

TEST(runner_rejects_undeclared_extensions) {
    Host h("op.extension('not.declared', {})");
    const rpc::Outcome o = h.init();
    CHECK_EQ(o.code, int(rpc::PluginError));
    CHECK(contains(o.message, "isn't listed in plugin.json"));
}

TEST(runner_refuses_host_calls_while_loading) {
    Host h("local rows = op.read('invoices')");
    const rpc::Outcome o = h.init();
    CHECK_EQ(o.code, int(rpc::PluginError));
    CHECK(contains(o.message, "while the plugin is loading"));
    CHECK(h.calls.empty());
}

TEST(runner_reports_plugin_errors) {
    Host h;
    CHECK(h.init().ok());
    rpc::Outcome o = h.eval("local x = nil; return x.field");
    CHECK_EQ(o.code, int(rpc::PluginError));
    CHECK(contains(o.message, "attempt to index a nil value"));
    o = h.eval("error('boom')");
    CHECK(contains(o.message, "boom"));
    CHECK(!contains(o.message, "stack traceback"));  // the user sees one line...
    o = h.eval("return function() end");
    CHECK(contains(o.message, "can't be converted"));
    CHECK(h.ok("return 'still alive'").asString() == "still alive");
    for (int i = 0; i < 50 && !contains(h.ch->log(), "stack traceback"); ++i) h.ch->poll(20ms);
    CHECK(contains(h.ch->log(), "stack traceback"));  // ...the log has the details
}

// ============================================================= runner: sandbox

TEST(sandbox_removes_dangerous_libraries) {
    Host h;
    CHECK(h.init().ok());
    const json::Value r = h.ok(R"(return {
        io = type(io), os = type(os), package = type(package), debug = type(debug),
        dofile = type(dofile), loadfile = type(loadfile), dump = type(string.dump),
        load = type(load), string = type(string), utf8 = type(utf8), coroutine = type(coroutine),
        math = type(math), table = type(table) })");
    for (const char* gone : {"io", "os", "package", "debug", "dofile", "loadfile", "dump"}) {
        CHECK_EQ(r.find(gone)->asString(), std::string("nil"));
        if (r.find(gone)->asString() != "nil") std::cerr << "  still present: " << gone << "\n";
    }
    for (const char* kept : {"load", "string", "utf8", "coroutine", "math", "table"})
        CHECK(r.find(kept)->asString() != "nil");
}

TEST(sandbox_refuses_bytecode) {
    Host h;
    CHECK(h.init().ok());
    // A binary chunk header, as a string and through a reader function.
    CHECK(contains(h.ok(R"(local f, err = load("\27Lua\84\0\25\147\13\10\26\10") return err)").asString(), "binary chunk"));
    CHECK(contains(h.ok(R"(
        local sent = false
        local f, err = load(function() if sent then return nil end sent = true return "\27Lua" end)
        return err)").asString(),
                   "binary chunk"));
    // Asking for mode "b" doesn't help: text is still all that loads.
    CHECK_EQ(h.ok(R"(return load("return 7", "x", "b")())").asInt(), 7);
    CHECK(contains(h.ok(R"(local f, err = load("\27Lua", "x", "b") return err)").asString(), "binary chunk"));
}

TEST(sandbox_require_loads_only_plugin_scripts) {
    Host h(kEvalMain, {},
           {{"lib/util.lua", "return { answer = 42 }"},
            {"lib/loop.lua", "return require('lib.loop')"},
            {"lib/broken.lua", "this is not lua"},
            {"secret.txt", "not a module"}});
    CHECK(h.init().ok());
    const json::Value r = h.ok("local a = require('lib.util'); local b = require('lib.util'); return { answer = a.answer, same = a == b }");
    CHECK_EQ(r.find("answer")->asInt(), 42);
    CHECK(r.find("same")->asBool());
    for (const char* bad : {"../x", "lib/util", "lib..util", ".lib", "lib.", "", "C:\\x", "lib.util.lua.."}) {
        const std::string msg = h.ok(std::string("local ok, err = pcall(require, ") + json::write(json::Value(bad)) + ") return tostring(err)").asString();
        CHECK(contains(msg, "invalid module name"));
    }
    CHECK(contains(h.ok("local ok, err = pcall(require, 'lib.missing') return err").asString(), "lib/missing.lua"));
    CHECK(contains(h.ok("local ok, err = pcall(require, 'secret') return err").asString(), "not found"));
    CHECK(contains(h.ok("local ok, err = pcall(require, 'lib.loop') return err").asString(), "require loop"));
    CHECK(contains(h.ok("local ok, err = pcall(require, 'lib.broken') return err").asString(), "lib/broken.lua"));
}

TEST(sandbox_reads_scripts_only_from_the_approved_copy) {
    // A module added or changed on disk after initialize is never seen.
    Host h(kEvalMain, {}, {{"lib/util.lua", "return { version = 1 }"}});
    CHECK(h.init().ok());
    writeFile(h.dir / "lib" / "util.lua", "return { version = 2 }");
    writeFile(h.dir / "lib" / "late.lua", "return {}");
    CHECK_EQ(h.ok("return require('lib.util').version").asInt(), 1);
    CHECK(contains(h.ok("local ok, err = pcall(require, 'lib.late') return err").asString(), "not found"));
}

TEST(sandbox_restricts_collectgarbage) {
    Host h;
    CHECK(h.init().ok());
    CHECK(!h.ok("return (pcall(collectgarbage, 'stop'))").asBool());
    CHECK(!h.ok("return (pcall(collectgarbage, 'generational'))").asBool());
    CHECK(h.ok("return collectgarbage('count') > 0").asBool());
    CHECK(h.ok("collectgarbage() return true").asBool());
}

TEST(sandbox_limits_memory) {
    Host h;
    json::Object params = h.initParams();
    params.set("limits", json::Object{{"memoryBytes", 16 << 20}});
    CHECK(h.init(params).ok());
    const rpc::Outcome o = h.eval("local t = {} for i = 1, 1e8 do t[i] = ('x'):rep(64) .. i end");
    CHECK_EQ(o.code, int(rpc::PluginError));
    CHECK(contains(o.message, "out of memory"));
    CHECK_EQ(h.ok("collectgarbage() return 2").asInt(), 2);  // and it keeps working
    // A single huge string is refused the same way.
    CHECK(contains(h.eval("return ('x'):rep(64 * 1024 * 1024)").message, "memory"));
}

TEST(sandbox_cancel_stops_loops) {
    for (const char* code : {"while true do end",
                             "while true do pcall(function() while true do end end) end",
                             "local co = coroutine.wrap(function() while true do end end) co()"}) {
        Host h;
        CHECK(h.init().ok());
        std::optional<rpc::Outcome> r;
        const auto id = h.ch->request("command/run", json::Object{{"id", "eval"}, {"context", json::Object{{"code", code}}}}, 0ms,
                                      [&](const rpc::Outcome& o) { r = o; });
        for (int i = 0; i < 10; ++i) h.ch->poll(20ms);
        CHECK(!r.has_value());
        h.ch->cancel(id);
        const auto start = std::chrono::steady_clock::now();
        CHECK_EQ(h.ok("return 42").asInt(), 42);  // the runner is free again
        CHECK(std::chrono::steady_clock::now() - start < 5s);
        if (std::chrono::steady_clock::now() - start >= 5s) std::cerr << "  slow to cancel: " << code << "\n";
    }
}

TEST(sandbox_timeout_cancels_and_recovers) {
    Host h;
    CHECK(h.init().ok());
    const rpc::Outcome o = h.eval("while true do end", 300ms);
    CHECK(o.status == rpc::Outcome::Status::Timeout);
    CHECK_EQ(h.ok("return 'next'").asString(), std::string("next"));
}

// ================================================================ runner: op API

TEST(op_json_round_trips) {
    Host h;
    CHECK(h.init().ok());
    const json::Value r = h.ok(R"(
        local t = op.json.decode('{"a":[1,null,3],"b":null,"c":{"d":"e"},"f":1.5,"g":[]}')
        return {
          len = #t.a, second_is_null = (t.a[2] == op.json.null), b_absent = (t.b == nil), d = t.c.d,
          f = t.f, empty_array = op.json.encode(t.g), empty_object = op.json.encode({}),
          marked = op.json.encode(op.json.array()), back = op.json.encode(t),
          int = math.type(op.json.decode('7')), float = math.type(op.json.decode('7.0')),
        })");
    CHECK_EQ(r.find("len")->asInt(), 3);
    CHECK(r.find("second_is_null")->asBool());
    CHECK(r.find("b_absent")->asBool());
    CHECK_EQ(r.find("d")->asString(), std::string("e"));
    CHECK_EQ(r.find("f")->asDouble(), 1.5);
    CHECK_EQ(r.find("empty_array")->asString(), std::string("[]"));
    CHECK_EQ(r.find("empty_object")->asString(), std::string("{}"));
    CHECK_EQ(r.find("marked")->asString(), std::string("[]"));
    CHECK(json::parse(r.find("back")->asString()) == json::parse(R"({"a":[1,null,3],"c":{"d":"e"},"f":1.5,"g":[]})"));
    CHECK_EQ(r.find("int")->asString(), std::string("integer"));
    CHECK_EQ(r.find("float")->asString(), std::string("float"));

    CHECK(contains(h.ok("local ok, e = pcall(op.json.encode, {1, 2, x = 3}) return e").asString(), "either only string keys"));
    CHECK(contains(h.ok("local ok, e = pcall(op.json.encode, {[1] = 'a', [3] = 'c'}) return e").asString(), "either only string keys"));
    CHECK(contains(h.ok("local ok, e = pcall(op.json.encode, {name = '\\255'}) return e").asString(), "at name: the string isn't valid UTF-8"));
    CHECK(contains(h.ok("local t = {} t.t = t local ok, e = pcall(op.json.encode, t) return e").asString(), "nested more than 64"));
    CHECK(contains(h.ok("local ok, e = pcall(op.json.encode, 0/0) return e").asString(), "finite"));
    CHECK(contains(h.ok("local ok, e = pcall(op.json.decode, '{\"a\":1,\"a\":2}') return e").asString(), "duplicate key"));
}

TEST(op_money_is_exact) {
    Host h;
    CHECK(h.init().ok());
    const json::Value r = h.ok(R"(return {
        op.money.add("1.10", "2.20", "-0.30"), op.money.sub("1.00", "2.50"),
        op.money.mul("10.00", "0.0725"), op.money.mul("10.00", 3), op.money.cmp("1.00", "0.99"),
        op.money.parse("12.34"), op.money.format(-5), (pcall(op.money.add, "1.234")),
        op.money.add("0.10", "0.20") })");
    const json::Array& a = r.asArray();
    CHECK_EQ(a[0].asString(), std::string("3.00"));
    CHECK_EQ(a[1].asString(), std::string("-1.50"));
    CHECK_EQ(a[2].asString(), std::string("0.73"));
    CHECK_EQ(a[3].asString(), std::string("30.00"));
    CHECK_EQ(a[4].asInt(), 1);
    CHECK_EQ(a[5].asInt(), 1234);
    CHECK_EQ(a[6].asString(), std::string("-0.05"));
    CHECK(!a[7].asBool());
    CHECK_EQ(a[8].asString(), std::string("0.30"));  // not 0.30000000000000004
    CHECK(contains(h.ok("local ok, e = pcall(op.money.add, '1.234') return e").asString(), "isn't an amount"));
}

TEST(op_codecs_and_time) {
    Host h;
    CHECK(h.init().ok());
    const json::Value r = h.ok(R"(return {
        b64 = op.base64.encode("foobar"), unb64 = op.base64.decode("Zm9vYmFy"), bad = op.base64.decode("!!") == nil,
        hex = op.hex.encode("\0\255"), sha = op.sha256("abc"), hmac = op.hmac_sha256("Jefe", "what do ya want for nothing?"),
        rnd = #op.random_bytes(16), now = op.now(), today = op.today() })");
    CHECK_EQ(r.find("b64")->asString(), std::string("Zm9vYmFy"));
    CHECK_EQ(r.find("unb64")->asString(), std::string("foobar"));
    CHECK(r.find("bad")->asBool());
    CHECK_EQ(r.find("hex")->asString(), std::string("00ff"));
    CHECK_EQ(r.find("sha")->asString(), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(r.find("hmac")->asString(), std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    CHECK_EQ(r.find("rnd")->asInt(), 16);
    CHECK_EQ(r.find("now")->asString().size(), std::size_t(20));  // 2026-10-01T23:45:43Z
    CHECK_EQ(r.find("now")->asString().back(), 'Z');
    CHECK_EQ(r.find("today")->asString().size(), std::size_t(10));
    CHECK(!h.ok("return (pcall(op.random_bytes, 2000))").asBool());
}

TEST(op_calls_the_host) {
    Host h;
    CHECK(h.init().ok());
    CHECK_EQ(h.ok("return op.read('invoices', { status = 'open' }).rows[1].total").asString(), std::string("10.00"));
    CHECK_EQ(h.calls.back().first, std::string("host/read"));
    CHECK_EQ(h.calls.back().second.find("query")->find("status")->asString(), std::string("open"));
    CHECK(contains(h.ok("local ok, e = pcall(op.read, 'ledger') return e").asString(), "read:ledger wasn't granted"));

    CHECK_EQ(h.ok("return op.propose{ summary = 'Import', changes = { { op = 'recordPayment', amount = '1.00' } } }.ids[1]").asInt(), 5);
    CHECK_EQ(h.calls.back().second.find("summary")->asString(), std::string("Import"));
}

TEST(op_store_goes_through_the_host) {
    Host h;
    CHECK(h.init().ok());
    CHECK(h.ok("op.store.set('apiKey', 'rk_123', { secret = true }) op.store.set('lastSync', '2026-09-30') return true").asBool());
    CHECK_EQ(h.store["apiKey"].first, std::string("rk_123"));
    CHECK(h.store["apiKey"].second);
    CHECK(!h.store["lastSync"].second);
    CHECK_EQ(h.ok("return op.store.get('lastSync')").asString(), std::string("2026-09-30"));
    CHECK(h.ok("op.store.delete('lastSync') return op.store.get('lastSync') == nil").asBool());
    CHECK(contains(h.ok("local ok, e = pcall(op.store.set, 'k', '\\255') return e").asString(), "UTF-8"));
}

TEST(op_logs_and_notifies) {
    Host h;
    CHECK(h.init().ok());
    h.ok("print('a', 1, nil) op.log('warn', 'careful') op.ui.notify('info', 'done') op.progress('half', 0.5) return true");
    CHECK(h.logged("info: a\t1\tnil"));
    CHECK(h.logged("warn: careful"));
    CHECK(h.logged("info: done"));
    CHECK_EQ(h.progress.size(), std::size_t(1));
    CHECK_EQ(h.progress[0].find("fraction")->asDouble(), 0.5);
    CHECK(h.ok("op.ui.show{ id = 'settings', title = 'Settings', body = {} } return true").asBool());
    CHECK_EQ(h.calls.back().first, std::string("ui/show"));
}

TEST(op_events_run_handlers) {
    Host h(std::string(kEvalMain) + R"(
        op.on('fileSaved', function(p) op.log('info', 'saved ' .. p.path) end)
        op.on('fileSaved', function(p) error('second handler fails') end)
        op.on('fileSaved', function(p) op.log('info', 'third ran') end))");
    CHECK(h.init().ok());
    h.ch->notify("event/fileSaved", json::Object{{"path", "books.obk"}});
    CHECK(h.ok("return 1").asInt() == 1);  // processed in order, after the event
    CHECK(h.logged("info: saved books.obk"));
    CHECK(h.logged("info: third ran"));  // one failing handler doesn't stop the rest
}

TEST(op_ui_events_reach_handlers) {
    Host h(std::string(kEvalMain) + R"(
        op.ui.on('settings', function(ev) return { clicked = ev.element, key = ev.values.key } end))");
    CHECK(h.init().ok());
    const rpc::Outcome o = h.call("ui/event", json::Object{{"viewId", "settings"}, {"element", "save"}, {"values", json::Object{{"key", "k1"}}}});
    CHECK(o.ok());
    CHECK_EQ(o.result.find("clicked")->asString(), std::string("save"));
    CHECK_EQ(h.call("ui/event", json::Object{{"viewId", "other"}}).code, int(rpc::MethodNotFound));
}

TEST(runner_queues_requests_while_a_handler_waits) {
    Host h;
    std::optional<rpc::Responder> pending;
    h.custom = [&](const std::string& method, const json::Value& params, rpc::Responder respond) {
        if (method != "host/read" || params.find("view")->asString() != "slow") return false;
        pending = respond;  // answered later, after another request has arrived
        return true;
    };
    CHECK(h.init().ok());
    std::vector<std::string> order;
    h.ch->request("command/run", json::Object{{"id", "eval"}, {"context", json::Object{{"code", "return op.read('slow').value"}}}}, 20s,
                  [&](const rpc::Outcome& o) { order.push_back(o.ok() ? o.result.asString() : "error: " + o.message); });
    for (int i = 0; i < 250 && !pending; ++i) h.ch->poll(20ms);
    CHECK(pending.has_value());
    h.ch->request("command/run", json::Object{{"id", "eval"}, {"context", json::Object{{"code", "return 'second'"}}}}, 20s,
                  [&](const rpc::Outcome& o) { order.push_back(o.ok() ? o.result.asString() : "error"); });
    for (int i = 0; i < 10; ++i) h.ch->poll(20ms);
    CHECK(order.empty());  // the second waits for the first
    pending->result(json::Object{{"value", "first"}});
    for (int i = 0; i < 250 && order.size() < 2; ++i) h.ch->poll(20ms);
    CHECK_EQ(order, (std::vector<std::string>{"first", "second"}));
}

// ================================================================= runner: HTTP

TEST(op_http_uses_the_real_transport_within_policy) {
    TestServer server;
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());
    Host h;
    CHECK(h.init(server.port()).ok());

    json::Value r = h.ok("local r = op.http.request{ url = '" + base + "/hello' } return { status = r.status, body = r.body, x = r.headers['x-test'], multi = r.headers['x-multi'] }");
    CHECK_EQ(r.find("status")->asInt(), 200);
    CHECK_EQ(r.find("body")->asString(), std::string("hi"));
    CHECK_EQ(r.find("x")->asString(), std::string("yes"));
    CHECK_EQ(r.find("multi")->asString(), std::string("a, b"));

    r = h.ok("return op.http.request{ method = 'POST', url = '" + base + "/echo', headers = { Authorization = 'Bearer t' }, body = 'payload' }.body");
    CHECK_EQ(r.asString(), std::string("POST|Bearer t|payload"));

    r = h.ok("local r = op.http.request{ url = '" + base + "/redirect-same' } return { body = r.body, url = r.url }");
    CHECK_EQ(r.find("body")->asString(), std::string("hi"));
    CHECK_EQ(r.find("url")->asString(), base + "/hello");

    r = h.ok("return op.http.request{ method = 'POST', url = '" + base + "/redirect-303', body = 'x' }.body");
    CHECK_EQ(r.asString(), std::string("GET|-|"));

    CHECK_EQ(h.ok("return op.http.request{ url = '" + base + "/nothing' }.status").asInt(), 404);

    const std::size_t before = server.seen().size();
    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = '" + base + "/redirect-away' }) return e").asString(), "refused a redirect"));
    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = 'https://example.com/' }) return e").asString(), "isn't one of this plugin's approved network hosts"));
    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = 'http://127.0.0.1:1/' }) return e").asString(), "isn't one of"));
    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = '" + base + "/hello', headers = { Host = 'x' } }) return e").asString(), "set by the runner"));
    CHECK_EQ(server.seen().size(), before + 1);  // only the first hop of redirect-away reached the server

    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = '" + base + "/big' }) return e").asString(), "larger than 16 MiB"));
}

TEST(op_http_needs_approved_hosts) {
    Host h;
    CHECK(h.init().ok());  // no network hosts approved
    CHECK(contains(h.ok("local ok, e = pcall(op.http.request, { url = 'https://example.com/' }) return e").asString(), "no approved network hosts"));
}

}  // namespace
