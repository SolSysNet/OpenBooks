#include "openbooks/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

// Windows hands narrow argv over in the ANSI code page; OpenBooks works in UTF-8.
static std::vector<std::string> utf8Arguments() {
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; wide && i < count; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    LocalFree(wide);
    return args;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    const std::vector<std::string> args = utf8Arguments();
#else
    const std::vector<std::string> args(argv + 1, argv + argc);
#endif
    return ob::runCli(args, std::cout, std::cerr, std::cin);
}
