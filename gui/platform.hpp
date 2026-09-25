#pragma once

// The few things the GUI needs from the operating system. Implemented per platform in
// platform_win32.cpp and platform_posix.cpp. All strings are UTF-8.

#include <optional>
#include <string>

namespace obgui {

struct FileFilter {
    const char* description;  // e.g. "OpenBooks files"
    const char* pattern;      // e.g. "*.obk"
};

// Native file dialogs. Return nullopt when cancelled or unavailable (the UI then falls
// back to a typed path).
std::optional<std::string> openFileDialog(const char* title, FileFilter filter);
std::optional<std::string> saveFileDialog(const char* title, FileFilter filter, const char* defaultExtension,
                                          const std::string& suggestedName);
bool nativeFileDialogsAvailable();

// Per-user settings directory (created on demand), e.g. %APPDATA%\OpenBooks.
std::string configDirectory();

// Candidate UI font files, best first.
const char* const* preferredFonts();      // null-terminated list
const char* const* preferredBoldFonts();  // null-terminated list

}  // namespace obgui
