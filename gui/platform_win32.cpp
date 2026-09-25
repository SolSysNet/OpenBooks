#include "platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <filesystem>
#include <vector>

namespace obgui {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "Description\0pattern\0All files\0*.*\0\0"
std::wstring filterString(FileFilter filter) {
    std::wstring f = widen(filter.description);
    f.push_back(L'\0');
    f += widen(filter.pattern);
    f.push_back(L'\0');
    f += L"All files";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

std::optional<std::string> runDialog(bool save, const char* title, FileFilter filter, const char* defaultExtension,
                                     const std::string& suggestedName) {
    std::vector<wchar_t> buffer(4096, L'\0');
    const std::wstring suggested = widen(suggestedName);
    if (!suggested.empty() && suggested.size() < buffer.size()) std::copy(suggested.begin(), suggested.end(), buffer.begin());
    const std::wstring filters = filterString(filter);
    const std::wstring wtitle = widen(title);
    const std::wstring ext = defaultExtension ? widen(defaultExtension) : std::wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filters.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return std::nullopt;
    return narrow(buffer.data());
}

}  // namespace

std::optional<std::string> openFileDialog(const char* title, FileFilter filter) {
    return runDialog(false, title, filter, nullptr, "");
}

std::optional<std::string> saveFileDialog(const char* title, FileFilter filter, const char* defaultExtension,
                                          const std::string& suggestedName) {
    return runDialog(true, title, filter, defaultExtension, suggestedName);
}

bool nativeFileDialogsAvailable() { return true; }

std::string configDirectory() {
    wchar_t* appData = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = std::filesystem::path(appData) / L"OpenBooks";
    } else {
        dir = std::filesystem::current_path();
    }
    CoTaskMemFree(appData);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return narrow(dir.wstring());
}

const char* const* preferredFonts() {
    static const char* const fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", nullptr};
    return fonts;
}

const char* const* preferredBoldFonts() {
    static const char* const fonts[] = {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf", nullptr};
    return fonts;
}

}  // namespace obgui
