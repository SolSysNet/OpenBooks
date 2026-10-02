#include "openplugin/store.hpp"

#include "openplugin/json.hpp"
#include "openplugin/manifest.hpp"

namespace opl {

namespace {

bool isHostKey(std::string_view key) { return !key.empty() && key[0] == '@'; }

void checkKey(std::string_view key) {
    if (key.empty()) throw StoreError("plugin data keys can't be empty");
    if (!json::isValidUtf8(key)) throw StoreError("plugin data keys must be valid UTF-8");
    for (char c : key)
        if (static_cast<unsigned char>(c) < 0x20) throw StoreError("plugin data keys can't contain control characters");
}

}  // namespace

const PluginStore::Entry* PluginStore::get(std::string_view pluginId, std::string_view key) const {
    if (isHostKey(key)) return nullptr;
    auto p = data_.find(pluginId);
    if (p == data_.end()) return nullptr;
    auto e = p->second.find(std::string(key));
    return e == p->second.end() ? nullptr : &e->second;
}

void PluginStore::set(const std::string& pluginId, const std::string& key, std::string value, bool secret) {
    if (isHostKey(key)) throw StoreError("keys starting with '@' are reserved for the app");
    put(pluginId, key, Entry{std::move(value), secret}, true);
}

bool PluginStore::erase(std::string_view pluginId, std::string_view key) {
    if (isHostKey(key)) return false;
    auto p = data_.find(pluginId);
    if (p == data_.end()) return false;
    const bool erased = p->second.erase(std::string(key)) > 0;
    if (p->second.empty()) data_.erase(p);
    return erased;
}

void PluginStore::setHost(const std::string& pluginId, const std::string& key, std::string value) {
    if (!isHostKey(key)) throw StoreError("host keys must start with '@'");
    put(pluginId, key, Entry{std::move(value), false}, true);
}

const std::string* PluginStore::getHost(std::string_view pluginId, std::string_view key) const {
    if (!isHostKey(key)) return nullptr;
    auto p = data_.find(pluginId);
    if (p == data_.end()) return nullptr;
    auto e = p->second.find(std::string(key));
    return e == p->second.end() ? nullptr : &e->second.value;
}

bool PluginStore::usedWithFile(std::string_view pluginId) const {
    const std::string* v = getHost(pluginId, "@use");
    return v && *v == "1";
}

void PluginStore::setUsedWithFile(const std::string& pluginId, bool used) {
    if (used) {
        setHost(pluginId, "@use", "1");
        return;
    }
    auto p = data_.find(pluginId);
    if (p == data_.end()) return;
    p->second.erase("@use");
    if (p->second.empty()) data_.erase(p);
}

void PluginStore::load(const std::string& pluginId, const std::string& key, std::string value, bool secret) {
    put(pluginId, key, Entry{std::move(value), secret}, false);
}

void PluginStore::put(const std::string& pluginId, const std::string& key, Entry entry, bool checkLimits) {
    if (!isValidPluginId(pluginId)) throw StoreError("invalid plugin id '" + pluginId + "'");
    checkKey(key);
    if (!json::isValidUtf8(entry.value)) throw StoreError("plugin data values must be valid UTF-8");
    if (checkLimits) {
        if (key.size() > limits_.maxKeyBytes)
            throw StoreError("plugin data keys are limited to " + std::to_string(limits_.maxKeyBytes) + " bytes");
        if (entry.value.size() > limits_.maxValueBytes)
            throw StoreError("plugin data values are limited to " + std::to_string(limits_.maxValueBytes / 1024) + " KiB");
        std::size_t used = bytesUsed(pluginId);
        const Entries& current = entries(pluginId);
        if (auto old = current.find(key); old != current.end()) used -= key.size() + old->second.value.size();
        if (used + key.size() + entry.value.size() > limits_.maxPluginBytes)
            throw StoreError("this plugin's data would exceed its " + std::to_string(limits_.maxPluginBytes >> 20) +
                             " MiB limit for one file");
    }
    data_[pluginId][key] = std::move(entry);
}

std::vector<std::string> PluginStore::plugins() const {
    std::vector<std::string> out;
    for (const auto& [id, entries] : data_) out.push_back(id);
    return out;
}

const PluginStore::Entries& PluginStore::entries(std::string_view pluginId) const {
    static const Entries none;
    auto p = data_.find(pluginId);
    return p == data_.end() ? none : p->second;
}

void PluginStore::erasePlugin(std::string_view pluginId) {
    auto p = data_.find(pluginId);
    if (p != data_.end()) data_.erase(p);
}

std::size_t PluginStore::bytesUsed(std::string_view pluginId) const {
    std::size_t total = 0;
    for (const auto& [key, e] : entries(pluginId)) total += key.size() + e.value.size();
    return total;
}

bool PluginStore::hasSecrets(std::string_view pluginId) const {
    for (const auto& [key, e] : entries(pluginId))
        if (e.secret) return true;
    return false;
}

}  // namespace opl
