#include "openbooks/paths.hpp"

#include "console.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ob {

namespace fs = std::filesystem;

std::string userConfigDirectory() {
    fs::path dir;
    const std::string overridden = environmentUtf8("OPENBOOKS_CONFIG_DIR");
    if (!overridden.empty()) {
        dir = fs::u8path(overridden);
    } else {
#ifdef _WIN32
        wchar_t* appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
            dir = fs::path(appData) / L"OpenBooks";
        else
            dir = fs::current_path();
        CoTaskMemFree(appData);
#else
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
            dir = fs::path(xdg) / "openbooks";
        } else if (const char* home = std::getenv("HOME"); home && *home) {
#ifdef __APPLE__
            dir = fs::path(home) / "Library" / "Application Support" / "OpenBooks";
#else
            dir = fs::path(home) / ".config" / "openbooks";
#endif
        } else {
            dir = fs::current_path();
        }
#endif
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.u8string();
}

std::string executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path().u8string();
        buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    const fs::path p = fs::canonical(fs::path(buf.data()), ec);
    return ec ? fs::path(buf.data()).parent_path().string() : p.parent_path().string();
#else
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.parent_path().string();
#endif
}

}  // namespace ob
