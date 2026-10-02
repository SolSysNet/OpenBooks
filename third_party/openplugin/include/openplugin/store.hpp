#pragma once

// Plugin data kept inside an app's own file (docs/design.md, section 11): for each plugin, a set of
// key/value strings, some marked secret. The app's file format writes one PLUGIN record per entry
// and keeps them whether or not the plugin is installed, so files round-trip without loss.
//
// Keys starting with '@' belong to the host (for example "@use", the per-file opt-in); plugins
// can't read or write them.

#include "openplugin/error.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace opl {

class StoreError : public Error {
public:
    using Error::Error;
};

struct StoreLimits {
    std::size_t maxKeyBytes = 256;
    std::size_t maxValueBytes = 64 * 1024;
    std::size_t maxPluginBytes = 1024 * 1024;  // keys plus values, per plugin
};

class PluginStore {
public:
    struct Entry {
        std::string value;
        bool secret = false;
        bool operator==(const Entry& o) const { return value == o.value && secret == o.secret; }
    };
    using Entries = std::map<std::string, Entry>;

    explicit PluginStore(StoreLimits limits = {}) : limits_(limits) {}

    // ---- Plugin access (the store/* protocol methods). Host keys ('@...') are refused.
    const Entry* get(std::string_view pluginId, std::string_view key) const;
    void set(const std::string& pluginId, const std::string& key, std::string value, bool secret);  // throws StoreError
    bool erase(std::string_view pluginId, std::string_view key);

    // ---- Host access
    // Host keys must start with '@'. Values are never secret.
    void setHost(const std::string& pluginId, const std::string& key, std::string value);
    const std::string* getHost(std::string_view pluginId, std::string_view key) const;
    // The per-file opt-in ("Use with this file").
    bool usedWithFile(std::string_view pluginId) const;
    void setUsedWithFile(const std::string& pluginId, bool used);

    // ---- Loading from and saving to the app's file
    // Adds an entry read from a file. Checks the plugin id, key and UTF-8 but not the size limits,
    // so a file written by a newer app with larger limits still opens. Throws StoreError.
    void load(const std::string& pluginId, const std::string& key, std::string value, bool secret);
    // Every plugin with data, sorted.
    std::vector<std::string> plugins() const;
    // That plugin's entries, sorted by key (host keys included). Empty if none.
    const Entries& entries(std::string_view pluginId) const;

    void erasePlugin(std::string_view pluginId);  // "Remove plugin data"
    std::size_t bytesUsed(std::string_view pluginId) const;
    bool hasSecrets(std::string_view pluginId) const;
    bool empty() const { return data_.empty(); }

    bool operator==(const PluginStore& o) const { return data_ == o.data_; }
    bool operator!=(const PluginStore& o) const { return !(*this == o); }

private:
    void put(const std::string& pluginId, const std::string& key, Entry entry, bool checkLimits);

    StoreLimits limits_;
    std::map<std::string, Entries, std::less<>> data_;
};

}  // namespace opl
