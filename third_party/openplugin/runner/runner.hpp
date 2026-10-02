#pragma once

// openplugin-runner: runs one Lua plugin in a sandbox and speaks the plugin protocol on
// stdin/stdout (docs/design.md, sections 7 and 8).

#include <filesystem>

namespace opl::runner {

// Runs until the host sends shutdown or closes stdin. Returns the process exit code.
int runPlugin(const std::filesystem::path& pluginDir);

}  // namespace opl::runner
