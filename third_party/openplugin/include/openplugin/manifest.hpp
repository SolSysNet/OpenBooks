#pragma once

// plugin.json: what a plugin is, what it needs, and how to run it (docs/design.md, section 4).
// Validation is strict, like the apps' file formats: unknown keys, unknown permissions and unsafe
// paths are errors, so nothing a user approved can mean something other than what they saw.

#include "openplugin/error.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opl {

class ManifestError : public Error {
public:
    using Error::Error;
};

enum class Runtime { Lua, Native };

struct CommandDecl {
    std::string id;
    std::string title;
    std::string menu;  // "Plugins" when not given; apps fall back to it for menus they don't have
};

struct Manifest {
    int schema = 1;
    std::string id;
    std::string name;
    std::string version;
    std::string publisher;
    std::string description;
    std::string homepage;  // empty, or an https:// URL (shown as text, never opened)
    std::vector<std::pair<std::string, std::string>> apps;  // app id -> version range
    int protocol = 1;
    Runtime runtime = Runtime::Lua;
    std::string main;  // Lua: entry script, relative to the plugin folder
    std::vector<std::string> runWindows, runLinux, runMacos;  // native: argument vectors
    std::vector<std::string> network;      // host patterns (see isValidHostPattern)
    std::vector<std::string> permissions;  // all known (see permissions())
    std::vector<std::string> extensions;
    std::vector<CommandDecl> commands;

    // The version range for `appId`, or nullptr if the plugin doesn't support that app.
    const std::string* appRange(std::string_view appId) const;
    // Native plugins: the argument vector for the platform this was compiled for (empty if none).
    const std::vector<std::string>& runForThisPlatform() const;
};

constexpr std::size_t kMaxManifestBytes = 64 * 1024;

Manifest parseManifest(std::string_view json);
// Reads <folder>/plugin.json. Throws ManifestError.
Manifest loadManifest(const std::filesystem::path& folder);

// ---- Permissions

struct PermissionInfo {
    const char* name;
    const char* description;  // plain language, for the consent screen
    bool high;                // high sensitivity: marked on the consent screen
};
const std::vector<PermissionInfo>& permissions();
const PermissionInfo* findPermission(std::string_view name);

// ---- Validation helpers

// Reverse-DNS style: [a-z0-9.-], 3 to 64 characters, no leading/trailing/double dots.
bool isValidPluginId(std::string_view id);

// A relative path inside the plugin folder: '/'-separated, no "..", ".", empty parts, backslashes,
// drive letters or control characters.
bool isSafeRelativePath(std::string_view path);

// A network host pattern: "api.example.com", "*.example.com" (exactly one label), "localhost",
// an IPv4 address, each optionally with ":port". Lowercase only.
bool isValidHostPattern(std::string_view pattern);
// Patterns the consent screen highlights: localhost, IP addresses and explicit ports (meant for
// local test servers during plugin development).
bool isDevelopmentHostPattern(std::string_view pattern);
// Whether `host` (lowercase, no trailing dot) on `port` is allowed by `pattern`. A pattern with no
// port allows only 443.
bool hostMatches(std::string_view pattern, std::string_view host, int port);

// ---- Versions: dot-separated numbers ("1", "0.6", "1.2.3")

std::optional<std::vector<int>> parseVersion(std::string_view s);
int compareVersions(const std::vector<int>& a, const std::vector<int>& b);  // missing parts are zero
// A range is space-separated comparators, all of which must hold: ">=0.6 <1.0", "=1.2", "1.2".
bool isValidVersionRange(std::string_view range);
bool versionInRange(std::string_view version, std::string_view range);

}  // namespace opl
