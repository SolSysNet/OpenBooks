#include "console.hpp"


#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#endif

namespace ob {

#ifdef _WIN32

bool readHiddenConsoleLine(std::string& line) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) return false;
    SetConsoleMode(input, (mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT)) | ENABLE_LINE_INPUT);
    std::wstring wide;
    wchar_t buffer[128];
    bool done = false;
    while (!done) {
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, 128, &read, nullptr) || read == 0) break;
        for (DWORD i = 0; i < read && !done; ++i) {
            if (buffer[i] == L'\r' || buffer[i] == L'\n') done = true;
            else wide.push_back(buffer[i]);
        }
    }
    SecureZeroMemory(buffer, sizeof buffer);
    SetConsoleMode(input, mode);
    line.clear();
    if (!wide.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        line.resize(static_cast<std::size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &line[0], n, nullptr, nullptr);
        SecureZeroMemory(&wide[0], wide.size() * sizeof(wchar_t));
    }
    return true;
}

std::string environmentUtf8(const char* name) {
    std::wstring wideName(name, name + std::char_traits<char>::length(name));  // names are ASCII
    const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (size == 0) return {};
    std::wstring wide(size, wchar_t{});
    const DWORD length = GetEnvironmentVariableW(wideName.c_str(), &wide[0], size);
    if (length == 0 || length >= size) return {};
    std::string value;
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        value.resize(static_cast<std::size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), &value[0], n, nullptr, nullptr);
    }
    SecureZeroMemory(&wide[0], wide.size() * sizeof(wchar_t));
    return value;
}

#else

bool readHiddenConsoleLine(std::string& line) {
    if (!isatty(STDIN_FILENO)) return false;
    termios original{};
    if (tcgetattr(STDIN_FILENO, &original) != 0) return false;
    termios hidden = original;
    hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    std::getline(std::cin, line);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    return true;
}

std::string environmentUtf8(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

#endif

}  // namespace ob
