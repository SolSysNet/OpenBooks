#include "openplugin/manifest.hpp"

#include "openplugin/json.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace opl {

// ------------------------------------------------------------------ permissions

const std::vector<PermissionInfo>& permissions() {
    static const std::vector<PermissionInfo> list = {
        {"read:company", "See your company or firm settings (name, address, fiscal year)", false},
        {"read:contacts", "See your customers, vendors and clients", false},
        {"read:invoices", "See your invoices and credit memos", false},
        {"read:bills", "See your bills", false},
        {"read:payments", "See payments you received and made", false},
        {"read:ledger", "See your accounts, balances and every transaction", true},
        {"read:return", "See your whole tax return, including names, Social Security numbers and dates of birth", true},
        {"read:result", "See every line of your computed tax return", true},
        {"read:projects", "See your projects, phases, staff and time", false},
        {"propose:payments", "Suggest payments for you to review and add", false},
        {"propose:transactions", "Suggest transactions (such as processing fees) for you to review and add", false},
        {"propose:contacts", "Suggest new contacts for you to review and add", false},
        {"store", "Keep its own settings and records inside your file", false},
        {"files", "Ask you to choose where to save a file it creates", false},
    };
    return list;
}

const PermissionInfo* findPermission(std::string_view name) {
    for (const PermissionInfo& p : permissions())
        if (name == p.name) return &p;
    return nullptr;
}

// -------------------------------------------------------------------- helpers

namespace {

bool isLowerAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool isDnsLabel(std::string_view label) {
    if (label.empty() || label.size() > 63) return false;
    if (label.front() == '-' || label.back() == '-') return false;
    return std::all_of(label.begin(), label.end(), [](char c) { return isLowerAlnum(c) || c == '-'; });
}

std::vector<std::string_view> splitView(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find(sep, start);
        parts.push_back(s.substr(start, p == std::string_view::npos ? std::string_view::npos : p - start));
        if (p == std::string_view::npos) return parts;
        start = p + 1;
    }
}

bool isIpv4(std::string_view s) {
    const auto parts = splitView(s, '.');
    if (parts.size() != 4) return false;
    for (std::string_view p : parts) {
        if (p.empty() || p.size() > 3 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        if (p.size() > 1 && p[0] == '0') return false;  // no octal-looking forms
        if (std::stoi(std::string(p)) > 255) return false;
    }
    return true;
}

struct HostPattern {
    bool wildcard = false;
    std::string_view host;  // without "*." and port
    int port = 443;
    bool explicitPort = false;
};

std::optional<HostPattern> splitHostPattern(std::string_view pattern) {
    HostPattern hp;
    std::string_view rest = pattern;
    const std::size_t colon = rest.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view port = rest.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port[0] == '0' ||
            !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        hp.port = std::stoi(std::string(port));
        if (hp.port < 1 || hp.port > 65535) return std::nullopt;
        hp.explicitPort = true;
        rest = rest.substr(0, colon);
    }
    if (rest.substr(0, 2) == "*.") {
        hp.wildcard = true;
        rest.remove_prefix(2);
    }
    hp.host = rest;
    return hp;
}

}  // namespace

bool isValidPluginId(std::string_view id) {
    if (id.size() < 3 || id.size() > 64) return false;
    if (id.front() == '.' || id.back() == '.' || id.front() == '-' || id.find("..") != std::string_view::npos) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return isLowerAlnum(c) || c == '.' || c == '-'; });
}

bool isSafeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > 256) return false;
    for (char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return false;
    }
    if (path.front() == '/') return false;
    for (std::string_view part : splitView(path, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
        if (part.back() == '.' || part.back() == ' ') return false;  // Windows silently strips these
    }
    return true;
}

bool isValidHostPattern(std::string_view pattern) {
    if (pattern.empty() || pattern.size() > 260) return false;
    const auto hp = splitHostPattern(pattern);
    if (!hp || hp->host.empty()) return false;
    if (hp->host == "localhost") return !hp->wildcard;
    if (isIpv4(hp->host)) return !hp->wildcard;
    if (hp->host.size() > 253) return false;
    const auto labels = splitView(hp->host, '.');
    if (labels.size() < 2) return false;  // no bare TLDs, and "*.com" can't happen
    if (!std::all_of(labels.begin(), labels.end(), isDnsLabel)) return false;
    // The last label can't be all digits (that would be a malformed IP, not a name).
    const std::string_view tld = labels.back();
    return !std::all_of(tld.begin(), tld.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool isDevelopmentHostPattern(std::string_view pattern) {
    const auto hp = splitHostPattern(pattern);
    if (!hp) return false;
    return hp->explicitPort || hp->host == "localhost" || isIpv4(hp->host);
}

bool hostMatches(std::string_view pattern, std::string_view host, int port) {
    if (!isValidHostPattern(pattern)) return false;
    const auto hp = splitHostPattern(pattern);
    if (port != hp->port) return false;
    if (!hp->wildcard) return host == hp->host;
    // "*.example.com" matches exactly one more label: "api.example.com", not "example.com" or "a.b.example.com".
    if (host.size() <= hp->host.size() + 1) return false;
    if (host.substr(host.size() - hp->host.size()) != hp->host) return false;
    if (host[host.size() - hp->host.size() - 1] != '.') return false;
    return isDnsLabel(host.substr(0, host.size() - hp->host.size() - 1));
}

// ------------------------------------------------------------------- versions

std::optional<std::vector<int>> parseVersion(std::string_view s) {
    if (s.empty() || s.size() > 32) return std::nullopt;
    std::vector<int> parts;
    for (std::string_view p : splitView(s, '.')) {
        if (p.empty() || p.size() > 6 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        if (p.size() > 1 && p[0] == '0') return std::nullopt;
        parts.push_back(std::stoi(std::string(p)));
    }
    if (parts.size() > 4) return std::nullopt;
    return parts;
}

int compareVersions(const std::vector<int>& a, const std::vector<int>& b) {
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

namespace {

struct Comparator {
    std::string_view op;
    std::vector<int> version;
};

std::optional<std::vector<Comparator>> parseRange(std::string_view range) {
    std::vector<Comparator> out;
    for (std::string_view token : splitView(range, ' ')) {
        if (token.empty()) continue;
        Comparator c;
        for (std::string_view op : {">=", "<=", ">", "<", "="}) {
            if (token.substr(0, op.size()) == op) {
                c.op = op;
                token.remove_prefix(op.size());
                break;
            }
        }
        if (c.op.empty()) c.op = "=";
        auto v = parseVersion(token);
        if (!v) return std::nullopt;
        c.version = std::move(*v);
        out.push_back(std::move(c));
    }
    if (out.empty()) return std::nullopt;
    return out;
}

}  // namespace

bool isValidVersionRange(std::string_view range) { return parseRange(range).has_value(); }

bool versionInRange(std::string_view version, std::string_view range) {
    const auto v = parseVersion(version);
    const auto r = parseRange(range);
    if (!v || !r) return false;
    for (const Comparator& c : *r) {
        const int cmp = compareVersions(*v, c.version);
        const bool ok = c.op == ">=" ? cmp >= 0 : c.op == "<=" ? cmp <= 0 : c.op == ">" ? cmp > 0 : c.op == "<" ? cmp < 0 : cmp == 0;
        if (!ok) return false;
    }
    return true;
}

// ------------------------------------------------------------------- manifest

const std::string* Manifest::appRange(std::string_view appId) const {
    for (const auto& [app, range] : apps)
        if (app == appId) return &range;
    return nullptr;
}

const std::vector<std::string>& Manifest::runForThisPlatform() const {
#if defined(_WIN32)
    return runWindows;
#elif defined(__APPLE__)
    return runMacos;
#else
    return runLinux;
#endif
}

namespace {

class Reader {
public:
    explicit Reader(const json::Object& obj, std::string where) : obj_(obj), where_(std::move(where)) {}

    [[noreturn]] void fail(const std::string& message) const { throw ManifestError("plugin.json: " + where_ + message); }

    const json::Value* get(std::string_view key) {
        seen_.insert(std::string(key));
        return obj_.find(key);
    }

    std::string string(std::string_view key, bool required, std::size_t maxLen) {
        const json::Value* v = get(key);
        if (!v) {
            if (required) fail("\"" + std::string(key) + "\" is required");
            return {};
        }
        if (!v->isString()) fail("\"" + std::string(key) + "\" must be a string");
        const std::string& s = v->asString();
        if (required && s.empty()) fail("\"" + std::string(key) + "\" can't be empty");
        if (s.size() > maxLen) fail("\"" + std::string(key) + "\" is longer than " + std::to_string(maxLen) + " bytes");
        for (char c : s)
            if (static_cast<unsigned char>(c) < 0x20) fail("\"" + std::string(key) + "\" contains a control character");
        return s;
    }

    int integer(std::string_view key) {
        const json::Value* v = get(key);
        if (!v) fail("\"" + std::string(key) + "\" is required");
        if (!v->isInteger()) fail("\"" + std::string(key) + "\" must be an integer");
        if (v->asInt() < 0 || v->asInt() > 1000000) fail("\"" + std::string(key) + "\" is out of range");
        return static_cast<int>(v->asInt());
    }

    std::vector<std::string> strings(std::string_view key, std::size_t maxCount, std::size_t maxLen) {
        const json::Value* v = get(key);
        if (!v) return {};
        if (!v->isArray()) fail("\"" + std::string(key) + "\" must be an array of strings");
        if (v->asArray().size() > maxCount) fail("\"" + std::string(key) + "\" has too many entries");
        std::vector<std::string> out;
        for (const json::Value& item : v->asArray()) {
            if (!item.isString()) fail("\"" + std::string(key) + "\" must be an array of strings");
            if (item.asString().size() > maxLen) fail("an entry in \"" + std::string(key) + "\" is too long");
            out.push_back(item.asString());
        }
        return out;
    }

    void noUnknownKeys() const {
        for (const auto& member : obj_)
            if (!seen_.count(member.first)) fail("unknown key \"" + member.first + "\"");
    }

private:
    const json::Object& obj_;
    std::string where_;
    std::set<std::string> seen_;
};

void requireUnique(const std::vector<std::string>& items, const char* what) {
    std::set<std::string> seen;
    for (const std::string& s : items)
        if (!seen.insert(s).second) throw ManifestError(std::string("plugin.json: \"") + what + "\" lists \"" + s + "\" twice");
}

}  // namespace

Manifest parseManifest(std::string_view text) {
    if (text.size() > kMaxManifestBytes) throw ManifestError("plugin.json is larger than 64 KiB");
    json::Value root;
    try {
        root = json::parse(text);
    } catch (const json::Error& e) {
        throw ManifestError(std::string("plugin.json: ") + e.what());
    }
    if (!root.isObject()) throw ManifestError("plugin.json must contain a JSON object");
    Reader r(root.asObject(), "");
    Manifest m;

    m.schema = r.integer("schema");
    if (m.schema != 1) r.fail("unsupported \"schema\" " + std::to_string(m.schema) + "; this app understands schema 1");

    m.id = r.string("id", true, 64);
    if (!isValidPluginId(m.id)) r.fail("\"id\" must be 3-64 characters of a-z, 0-9, '.' and '-' (e.g. \"org.example.my-plugin\")");
    m.name = r.string("name", true, 80);
    m.version = r.string("version", true, 32);
    if (!parseVersion(m.version)) r.fail("\"version\" must be numbers separated by dots (e.g. \"1.2.0\")");
    m.publisher = r.string("publisher", true, 80);
    m.description = r.string("description", false, 1000);
    m.homepage = r.string("homepage", false, 500);
    if (!m.homepage.empty() && m.homepage.rfind("https://", 0) != 0) r.fail("\"homepage\" must start with https://");

    const json::Value* apps = r.get("apps");
    if (!apps || !apps->isObject() || apps->asObject().empty())
        r.fail("\"apps\" must name at least one app and its supported versions, e.g. {\"openbooks\": \">=0.6\"}");
    for (const auto& [app, range] : apps->asObject()) {
        if (app.empty() || app.size() > 32 || !std::all_of(app.begin(), app.end(), [](char c) { return isLowerAlnum(c) || c == '-'; }))
            r.fail("\"apps\" has an invalid app name \"" + app + "\"");
        if (!range.isString() || !isValidVersionRange(range.asString()))
            r.fail("\"apps\": \"" + app + "\" needs a version range such as \">=0.6 <1.0\"");
        m.apps.emplace_back(app, range.asString());
    }

    m.protocol = r.integer("protocol");

    const std::string runtime = r.string("runtime", true, 16);
    if (runtime == "lua") {
        m.runtime = Runtime::Lua;
        m.main = r.string("main", true, 256);
        if (!isSafeRelativePath(m.main) || m.main.size() < 5 || m.main.substr(m.main.size() - 4) != ".lua")
            r.fail("\"main\" must be a .lua file inside the plugin folder");
        if (r.get("run")) r.fail("\"run\" is only for native plugins");
    } else if (runtime == "native") {
        m.runtime = Runtime::Native;
        if (r.get("main")) r.fail("\"main\" is only for Lua plugins");
        const json::Value* run = r.get("run");
        if (!run || !run->isObject() || run->asObject().empty())
            r.fail("native plugins need \"run\" with an argument list for at least one of windows, linux, macos");
        Reader rr(run->asObject(), "\"run\": ");
        m.runWindows = rr.strings("windows", 64, 4096);
        m.runLinux = rr.strings("linux", 64, 4096);
        m.runMacos = rr.strings("macos", 64, 4096);
        rr.noUnknownKeys();
        for (const std::vector<std::string>* argv : {&m.runWindows, &m.runLinux, &m.runMacos}) {
            if (argv->empty()) continue;
            std::string exe = (*argv)[0];
            if (exe.rfind("./", 0) == 0) exe = exe.substr(2);
            if (!isSafeRelativePath(exe)) r.fail("\"run\": the program must be a file inside the plugin folder");
        }
    } else {
        r.fail("\"runtime\" must be \"lua\" or \"native\"");
    }

    m.network = r.strings("network", 32, 260);
    for (const std::string& h : m.network)
        if (!isValidHostPattern(h))
            r.fail("\"network\": \"" + h + "\" isn't a valid host (use lowercase names like \"api.example.com\" or \"*.example.com\")");
    requireUnique(m.network, "network");

    m.permissions = r.strings("permissions", 64, 64);
    for (const std::string& p : m.permissions)
        if (!findPermission(p)) r.fail("\"permissions\": unknown permission \"" + p + "\"");
    requireUnique(m.permissions, "permissions");

    m.extensions = r.strings("extensions", 32, 64);
    for (const std::string& e : m.extensions) {
        if (e.empty() || !std::all_of(e.begin(), e.end(), [](char c) { return isLowerAlnum(c) || c == '.' || c == '-'; }))
            r.fail("\"extensions\": invalid extension point name \"" + e + "\"");
    }
    requireUnique(m.extensions, "extensions");

    if (const json::Value* commands = r.get("commands")) {
        if (!commands->isArray()) r.fail("\"commands\" must be an array");
        if (commands->asArray().size() > 32) r.fail("\"commands\" has too many entries");
        std::set<std::string> ids;
        for (const json::Value& c : commands->asArray()) {
            if (!c.isObject()) r.fail("each entry in \"commands\" must be an object");
            Reader cr(c.asObject(), "\"commands\": ");
            CommandDecl cmd;
            cmd.id = cr.string("id", true, 64);
            if (!std::all_of(cmd.id.begin(), cmd.id.end(), [](char ch) { return isLowerAlnum(ch) || ch == '.' || ch == '-'; }))
                cr.fail("invalid command id \"" + cmd.id + "\"");
            if (!ids.insert(cmd.id).second) cr.fail("command id \"" + cmd.id + "\" is used twice");
            cmd.title = cr.string("title", true, 80);
            cmd.menu = cr.string("menu", false, 32);
            if (cmd.menu.empty()) cmd.menu = "Plugins";
            cr.noUnknownKeys();
            m.commands.push_back(std::move(cmd));
        }
    }

    r.noUnknownKeys();
    return m;
}

Manifest loadManifest(const std::filesystem::path& folder) {
    const std::filesystem::path file = folder / "plugin.json";
    std::error_code ec;
    if (std::filesystem::is_symlink(file, ec)) throw ManifestError("plugin.json is a link; it must be a regular file");
    if (!std::filesystem::is_regular_file(file, ec)) throw ManifestError("no plugin.json in this folder");
    if (std::filesystem::file_size(file, ec) > kMaxManifestBytes) throw ManifestError("plugin.json is larger than 64 KiB");
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ManifestError("cannot read plugin.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseManifest(text);
}

}  // namespace opl
