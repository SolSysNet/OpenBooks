#pragma once

#include <string>

namespace ob {

// The per-user settings folder, created on demand. All strings are UTF-8.
//   Windows: %APPDATA%\OpenBooks
//   Linux:   $XDG_CONFIG_HOME/openbooks, or ~/.config/openbooks
//   macOS:   ~/Library/Application Support/OpenBooks
// OPENBOOKS_CONFIG_DIR overrides it (portable installs, tests).
std::string userConfigDirectory();

// The folder holding the running program (where openplugin-runner is installed).
std::string executableDirectory();

}  // namespace ob
