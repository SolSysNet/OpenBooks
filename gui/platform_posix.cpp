#include "platform.hpp"

#include <cstdlib>
#include <filesystem>

namespace obgui {

// Native dialogs are not wired up on Linux/macOS yet; the UI falls back to typed paths.
std::optional<std::string> openFileDialog(const char*, FileFilter) { return std::nullopt; }

std::optional<std::string> saveFileDialog(const char*, FileFilter, const char*, const std::string&) {
    return std::nullopt;
}

bool nativeFileDialogsAvailable() { return false; }

// Not implemented yet on Linux/macOS (would shell out to xdg-open / open).
bool openWithDefaultApp(const std::string&) { return false; }

std::string configDirectory() {
    std::filesystem::path dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        dir = std::filesystem::path(xdg) / "openbooks";
    } else if (const char* home = std::getenv("HOME"); home && *home) {
#ifdef __APPLE__
        dir = std::filesystem::path(home) / "Library" / "Application Support" / "OpenBooks";
#else
        dir = std::filesystem::path(home) / ".config" / "openbooks";
#endif
    } else {
        dir = std::filesystem::current_path();
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

const char* const* preferredFonts() {
    static const char* const fonts[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        nullptr,
    };
    return fonts;
}

const char* const* preferredBoldFonts() {
    static const char* const fonts[] = {
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        nullptr,
    };
    return fonts;
}

}  // namespace obgui
