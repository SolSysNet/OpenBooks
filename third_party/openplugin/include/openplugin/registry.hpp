#pragma once

// Which plugins are installed, which the user enabled, and exactly what they approved
// (docs/design.md, section 5).
//
// A plugin is approved together with a hash of every file in its folder. If any file changes (an
// update or tampering) the plugin needs approval again before it can run.

#include "openplugin/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opl {

struct AppIdentity {
    std::string id;       // "openbooks", "opentax", "openpractice"
    std::string version;  // "0.6.0"
    int protocolMin = 1;
    int protocolMax = 1;
};

// What the user approved for one plugin.
struct Grant {
    std::string version;
    std::string hash;
    std::vector<std::string> permissions;  // granted (may be fewer than requested)
    std::vector<std::string> network;      // approved hosts (may be fewer than requested)
    std::vector<std::string> requestedPermissions;  // what the plugin asked for at the time
    std::vector<std::string> requestedNetwork;
};

enum class PluginStatus {
    NotEnabled,     // found, never approved (or disabled)
    Enabled,        // approved, and the files still match
    NeedsApproval,  // approved once, but the files changed since
    Incompatible,   // valid, but not for this app, app version or protocol
    Invalid,        // broken manifest, unsafe folder contents, or a duplicate id
};

const char* toString(PluginStatus s);

struct PluginEntry {
    std::filesystem::path folder;
    std::optional<Manifest> manifest;  // absent when Invalid because the manifest is unusable
    std::string hash;                  // empty when the folder couldn't be hashed
    PluginStatus status = PluginStatus::NotEnabled;
    std::string detail;                // why Invalid / Incompatible / NeedsApproval
    std::optional<Grant> grant;
    // NeedsApproval: what the new version asks for beyond what was requested last time.
    std::vector<std::string> newPermissions;
    std::vector<std::string> newNetwork;

    const std::string& id() const;  // manifest id (empty if no manifest)
};

// Hash of a plugin folder: SHA-256 over "<sha256 of file>  <relative/path>\n" for every file,
// sorted by path. Links of any kind are refused, as are more than kMaxPluginFiles files or
// kMaxPluginBytes in total. The OS's own folder clutter (.DS_Store, Thumbs.db, desktop.ini) is
// skipped so that just viewing the folder doesn't change the hash.
constexpr std::size_t kMaxPluginFiles = 2000;
constexpr std::uintmax_t kMaxPluginBytes = 64u << 20;
std::string hashPluginFolder(const std::filesystem::path& folder);

// The plugin's files read into memory, sorted by path, with the same rules and limits. The runner
// loads scripts only from this copy after checking hashPluginFiles() against the approved hash, so
// nothing can be swapped on disk between the check and the use.
struct PluginFile {
    std::string path;  // '/'-separated, relative to the plugin folder
    std::string content;
};
std::vector<PluginFile> readPluginFolder(const std::filesystem::path& folder);
std::string hashPluginFiles(const std::vector<PluginFile>& files);
// The files that go into the hash, as '/'-separated relative paths (for "Show files").
std::vector<std::string> listPluginFiles(const std::filesystem::path& folder);

class Registry {
public:
    // `pluginsDir`: the per-user plugin folder. `registryFile`: where approvals are kept (beside the
    // app's other settings). Neither needs to exist yet.
    Registry(std::filesystem::path pluginsDir, std::filesystem::path registryFile, AppIdentity app);

    // Reads the registry file. A missing file is an empty registry; a corrupt one throws opl::Error
    // (and the registry stays empty, so every plugin shows as not enabled).
    void load();

    // Every plugin folder, with its status. Sorted by name, then folder.
    std::vector<PluginEntry> scan() const;

    // Approves `entry` with these permissions and hosts, which must be subsets of what it requested.
    // The folder is hashed again and must still match entry.hash, so files can't change between the
    // consent screen and approval. Saves the registry.
    void enable(const PluginEntry& entry, const std::vector<std::string>& permissions,
                const std::vector<std::string>& network);
    void disable(const std::string& pluginId);  // saves the registry

    const Grant* grant(const std::string& pluginId) const;
    const std::filesystem::path& pluginsDir() const { return pluginsDir_; }
    const AppIdentity& app() const { return app_; }

private:
    void save() const;

    std::filesystem::path pluginsDir_;
    std::filesystem::path registryFile_;
    AppIdentity app_;
    std::map<std::string, Grant> grants_;
};

}  // namespace opl
