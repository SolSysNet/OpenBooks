// openplugin-runner --plugin <folder>
//
// Started by OpenBooks, OpenTax or OpenPractice, one process per active Lua plugin. It speaks the
// plugin protocol on stdin/stdout and writes its log to stderr. See docs/design.md.

#include "runner.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

const char* kUsage = "usage: openplugin-runner --plugin <folder>\n"
                     "Runs a Lua plugin for OpenBooks, OpenTax or OpenPractice. The app starts this; it isn't\n"
                     "meant to be run by hand.\n";

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> args;
#ifdef _WIN32
    // Protocol lines end in "\n", never "\r\n".
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    // argv is in the ANSI code page; read the real (UTF-16) command line instead.
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; i < wargc; ++i) args.emplace_back(wargv[i]);
    LocalFree(wargv);
    (void)argc;
    (void)argv;
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif

    std::filesystem::path plugin;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string a = args[i].u8string();
        if (a == "--version") {
            std::printf("openplugin-runner 0.1.0 (Lua 5.4.9, protocol 1)\n");
            return 0;
        }
        if (a == "--plugin" && i + 1 < args.size()) {
            plugin = args[++i];
            continue;
        }
        std::fputs(kUsage, stderr);
        return 2;
    }
    if (plugin.empty() || !plugin.is_absolute()) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    return opl::runner::runPlugin(plugin);
}
