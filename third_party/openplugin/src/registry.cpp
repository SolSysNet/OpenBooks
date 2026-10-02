#include "openplugin/registry.hpp"

#include "openplugin/json.hpp"
#include "openplugin/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;

namespace opl {

const char* toString(PluginStatus s) {
    switch (s) {
        case PluginStatus::NotEnabled: return "not enabled";
        case PluginStatus::Enabled: return "enabled";
        case PluginStatus::NeedsApproval: return "needs approval";
        case PluginStatus::Incompatible: return "incompatible";
        case PluginStatus::Invalid: return "invalid";
    }
    return "?";
}

const std::string& PluginEntry::id() const {
    static const std::string none;
    return manifest ? manifest->id : none;
}

// ------------------------------------------------------------------ hashing

namespace {

bool isOsClutter(const std::string& name) {
    return name == ".DS_Store" || name == "Thumbs.db" || name == "desktop.ini";
}

struct FileRef {
    std::string relative;  // '/'-separated UTF-8
    fs::path path;
};

std::vector<FileRef> collectFiles(const fs::path& folder) {
    std::error_code ec;
    if (fs::is_symlink(folder, ec)) throw Error("the plugin folder is a link");
    if (!fs::is_directory(folder, ec)) throw Error("the plugin folder doesn't exist");
    std::vector<FileRef> files;
    std::uintmax_t total = 0;
    fs::recursive_directory_iterator it(folder, fs::directory_options::none, ec);
    if (ec) throw Error("cannot read the plugin folder: " + ec.message());
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) throw Error("cannot read the plugin folder: " + ec.message());
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().u8string();
        if (entry.is_symlink(ec)) throw Error("the plugin folder contains a link (" + name + "), which isn't allowed");
        if (entry.is_directory(ec)) continue;
        if (!entry.is_regular_file(ec)) throw Error("the plugin folder contains something that isn't a file (" + name + ")");
        if (isOsClutter(name)) continue;
        const std::string relative = fs::relative(entry.path(), folder, ec).generic_u8string();
        if (ec || relative.empty() || relative.rfind("..", 0) == 0) throw Error("cannot read the plugin folder");
        total += entry.file_size(ec);
        if (files.size() >= kMaxPluginFiles) throw Error("the plugin has more than " + std::to_string(kMaxPluginFiles) + " files");
        if (total > kMaxPluginBytes) throw Error("the plugin is larger than " + std::to_string(kMaxPluginBytes >> 20) + " MiB");
        files.push_back({relative, entry.path()});
    }
    std::sort(files.begin(), files.end(), [](const FileRef& a, const FileRef& b) { return a.relative < b.relative; });
    return files;
}

}  // namespace

std::vector<PluginFile> readPluginFolder(const fs::path& folder) {
    std::vector<PluginFile> out;
    for (const FileRef& f : collectFiles(folder)) {
        std::ifstream in(f.path, std::ios::binary);
        if (!in) throw Error("cannot read '" + f.relative + "' in the plugin folder");
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) throw Error("failed while reading '" + f.relative + "' in the plugin folder");
        out.push_back({f.relative, std::move(content)});
    }
    return out;
}

std::string hashPluginFiles(const std::vector<PluginFile>& files) {
    Sha256 h;
    for (const PluginFile& f : files) {
        h.update(sha256Hex(f.content));
        h.update("  ");
        h.update(f.path);
        h.update("\n");
    }
    const auto digest = h.finish();
    return toHex(digest.data(), digest.size());
}

// One implementation of the hash, so the registry and the runner can never disagree.
std::string hashPluginFolder(const fs::path& folder) { return hashPluginFiles(readPluginFolder(folder)); }

std::vector<std::string> listPluginFiles(const fs::path& folder) {
    std::vector<std::string> out;
    for (const FileRef& f : collectFiles(folder)) out.push_back(f.relative);
    return out;
}

// ----------------------------------------------------------------- registry

namespace {

json::Value stringArray(const std::vector<std::string>& items) {
    json::Array a;
    for (const std::string& s : items) a.emplace_back(s);
    return json::Value(std::move(a));
}

std::vector<std::string> readStringArray(const json::Value* v) {
    std::vector<std::string> out;
    if (!v) return out;
    for (const json::Value& item : v->asArray()) out.push_back(item.asString());
    return out;
}

std::vector<std::string> missingFrom(const std::vector<std::string>& wanted, const std::vector<std::string>& have) {
    std::vector<std::string> out;
    for (const std::string& w : wanted)
        if (std::find(have.begin(), have.end(), w) == have.end()) out.push_back(w);
    return out;
}

}  // namespace

Registry::Registry(fs::path pluginsDir, fs::path registryFile, AppIdentity app)
    : pluginsDir_(std::move(pluginsDir)), registryFile_(std::move(registryFile)), app_(std::move(app)) {}

void Registry::load() {
    grants_.clear();
    std::error_code ec;
    if (!fs::exists(registryFile_, ec)) return;
    std::ifstream in(registryFile_, std::ios::binary);
    if (!in) throw Error("cannot read the plugin settings '" + registryFile_.u8string() + "'");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::map<std::string, Grant> grants;
    try {
        const json::Value root = json::parse(text);
        if (root.find("schema") == nullptr || root.find("schema")->asInt() != 1)
            throw Error("unsupported plugin settings version");
        const json::Value* plugins = root.find("plugins");
        if (plugins) {
            for (const auto& [id, g] : plugins->asObject()) {
                if (!isValidPluginId(id)) throw Error("invalid plugin id '" + id + "'");
                Grant grant;
                grant.version = g.find("version") ? g.find("version")->asString() : "";
                grant.hash = g.find("hash") ? g.find("hash")->asString() : "";
                grant.permissions = readStringArray(g.find("permissions"));
                grant.network = readStringArray(g.find("network"));
                grant.requestedPermissions = readStringArray(g.find("requestedPermissions"));
                grant.requestedNetwork = readStringArray(g.find("requestedNetwork"));
                if (grant.hash.size() != 64) throw Error("plugin '" + id + "' has no valid hash");
                grants[id] = std::move(grant);
            }
        }
    } catch (const json::Error& e) {
        throw Error("the plugin settings file '" + registryFile_.u8string() + "' is damaged (" + e.what() +
                    "); every plugin is disabled until you enable it again");
    } catch (const Error& e) {
        throw Error("the plugin settings file '" + registryFile_.u8string() + "' is damaged (" + e.what() +
                    "); every plugin is disabled until you enable it again");
    }
    grants_ = std::move(grants);
}

void Registry::save() const {
    json::Object plugins;
    for (const auto& [id, g] : grants_) {
        plugins.set(id, json::Object{
                            {"version", g.version},
                            {"hash", g.hash},
                            {"permissions", stringArray(g.permissions)},
                            {"network", stringArray(g.network)},
                            {"requestedPermissions", stringArray(g.requestedPermissions)},
                            {"requestedNetwork", stringArray(g.requestedNetwork)},
                        });
    }
    const std::string text = json::writePretty(json::Object{{"schema", 1}, {"plugins", std::move(plugins)}});

    // Atomic replace, like the apps' own files.
    std::error_code ec;
    if (registryFile_.has_parent_path()) fs::create_directories(registryFile_.parent_path(), ec);
    fs::path tmp = registryFile_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot write '" + tmp.u8string() + "'");
        out << text;
        out.flush();
        if (!out) throw Error("failed while writing '" + tmp.u8string() + "'");
    }
    fs::rename(tmp, registryFile_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw Error("cannot save the plugin settings '" + registryFile_.u8string() + "'");
    }
}

std::vector<PluginEntry> Registry::scan() const {
    std::vector<PluginEntry> entries;
    std::error_code ec;
    if (!fs::is_directory(pluginsDir_, ec)) return entries;

    for (fs::directory_iterator it(pluginsDir_, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) && !it->is_symlink(ec)) continue;  // loose files are ignored
        PluginEntry e;
        e.folder = it->path();
        try {
            if (it->is_symlink(ec)) throw Error("the plugin folder is a link");
            e.manifest = loadManifest(e.folder);
            e.hash = hashPluginFolder(e.folder);
        } catch (const Error& err) {
            e.status = PluginStatus::Invalid;
            e.detail = err.what();
        }
        entries.push_back(std::move(e));
    }

    // Two folders claiming one id: neither can be trusted to be "the" plugin the user approved.
    std::map<std::string, int> idCount;
    for (const PluginEntry& e : entries)
        if (e.manifest) ++idCount[e.manifest->id];

    for (PluginEntry& e : entries) {
        if (e.status == PluginStatus::Invalid) continue;
        const Manifest& m = *e.manifest;
        if (idCount[m.id] > 1) {
            e.status = PluginStatus::Invalid;
            e.detail = "more than one installed plugin uses the id \"" + m.id + "\"";
            continue;
        }
        const std::string* range = m.appRange(app_.id);
        if (!range) {
            e.status = PluginStatus::Incompatible;
            e.detail = "this plugin isn't made for this app";
            continue;
        }
        if (!versionInRange(app_.version, *range)) {
            e.status = PluginStatus::Incompatible;
            e.detail = "this plugin needs app version " + *range + " (this is " + app_.version + ")";
            continue;
        }
        if (m.protocol < app_.protocolMin || m.protocol > app_.protocolMax) {
            e.status = PluginStatus::Incompatible;
            e.detail = "this plugin uses protocol " + std::to_string(m.protocol) + ", which this app version doesn't support";
            continue;
        }
        if (m.runtime == Runtime::Native && m.runForThisPlatform().empty()) {
            e.status = PluginStatus::Incompatible;
            e.detail = "this plugin has no version for this operating system";
            continue;
        }
        auto g = grants_.find(m.id);
        if (g == grants_.end()) {
            e.status = PluginStatus::NotEnabled;
            continue;
        }
        e.grant = g->second;
        if (g->second.hash != e.hash) {
            e.status = PluginStatus::NeedsApproval;
            e.detail = g->second.version == m.version
                           ? "the plugin's files changed since you approved it"
                           : "the plugin was updated from " + g->second.version + " to " + m.version;
            e.newPermissions = missingFrom(m.permissions, g->second.requestedPermissions);
            e.newNetwork = missingFrom(m.network, g->second.requestedNetwork);
            continue;
        }
        e.status = PluginStatus::Enabled;
    }

    std::sort(entries.begin(), entries.end(), [](const PluginEntry& a, const PluginEntry& b) {
        const std::string an = a.manifest ? a.manifest->name : "";
        const std::string bn = b.manifest ? b.manifest->name : "";
        if (an != bn) return an < bn;
        return a.folder < b.folder;
    });
    return entries;
}

void Registry::enable(const PluginEntry& entry, const std::vector<std::string>& permissions,
                      const std::vector<std::string>& network) {
    if (!entry.manifest || entry.status == PluginStatus::Invalid || entry.status == PluginStatus::Incompatible)
        throw Error("this plugin can't be enabled: " + entry.detail);
    const Manifest& m = *entry.manifest;
    for (const std::string& p : permissions)
        if (std::find(m.permissions.begin(), m.permissions.end(), p) == m.permissions.end())
            throw Error("the plugin didn't ask for the permission \"" + p + "\"");
    for (const std::string& h : network)
        if (std::find(m.network.begin(), m.network.end(), h) == m.network.end())
            throw Error("the plugin didn't ask to contact \"" + h + "\"");

    // Re-read everything: what gets approved is what the user was shown.
    if (hashPluginFolder(entry.folder) != entry.hash) throw Error("the plugin's files changed while you were reviewing it");
    if (loadManifest(entry.folder).id != m.id) throw Error("the plugin's files changed while you were reviewing it");

    Grant g;
    g.version = m.version;
    g.hash = entry.hash;
    g.permissions = permissions;
    g.network = network;
    g.requestedPermissions = m.permissions;
    g.requestedNetwork = m.network;
    auto previous = grants_.find(m.id);
    std::optional<Grant> old;
    if (previous != grants_.end()) old = previous->second;
    grants_[m.id] = std::move(g);
    try {
        save();
    } catch (...) {
        if (old) grants_[m.id] = *old;
        else grants_.erase(m.id);
        throw;
    }
}

void Registry::disable(const std::string& pluginId) {
    auto it = grants_.find(pluginId);
    if (it == grants_.end()) return;
    const Grant old = it->second;
    grants_.erase(it);
    try {
        save();
    } catch (...) {
        grants_[pluginId] = old;
        throw;
    }
}

const Grant* Registry::grant(const std::string& pluginId) const {
    auto it = grants_.find(pluginId);
    return it == grants_.end() ? nullptr : &it->second;
}

}  // namespace opl
