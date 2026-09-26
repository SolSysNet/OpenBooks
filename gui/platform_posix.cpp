// Linux / macOS platform services.
//
// Native file dialogs are provided by helper programs the desktop already ships:
//   * macOS: osascript ("choose file" / "choose file name" show the system NSOpenPanel / NSSavePanel)
//   * Linux: kdialog on KDE, zenity elsewhere (GNOME, Xfce, ...)
// They are launched with posix_spawnp and an explicit argument vector: no shell is ever
// involved, so titles and file names can't be interpreted as commands. Nothing is linked
// in and nothing touches the network. When no helper exists the UI falls back to typed paths.

#include "platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace obgui {
namespace {

struct ProcessResult {
    int exitCode = -1;
    std::string output;  // stdout
};

// Runs argv[0] (looked up in PATH) with the given arguments, without a shell.
// When captureOutput is false the child's stdout goes to /dev/null and we do not wait for
// grandchildren (a viewer launched by xdg-open keeps running on its own).
std::optional<ProcessResult> runProcess(const std::vector<std::string>& args, bool captureOutput) {
    if (args.empty()) return std::nullopt;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int pipeFds[2] = {-1, -1};
    if (captureOutput && pipe(pipeFds) != 0) return std::nullopt;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (captureOutput) {
        posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
        posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (captureOutput) close(pipeFds[1]);
    if (rc != 0) {
        if (captureOutput) close(pipeFds[0]);
        return std::nullopt;
    }

    ProcessResult result;
    if (captureOutput) {
        char buffer[4096];
        while (true) {
            const ssize_t n = read(pipeFds[0], buffer, sizeof buffer);
            if (n > 0) {
                result.output.append(buffer, static_cast<std::size_t>(n));
                if (result.output.size() > 64 * 1024) break;  // a path is never this long
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
        close(pipeFds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

bool inPath(const char* program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string dirs = path;
    std::size_t start = 0;
    while (start <= dirs.size()) {
        const std::size_t end = dirs.find(':', start);
        const std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + program;
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// The chosen path, or nullopt when cancelled or the helper failed.
std::optional<std::string> pathFrom(const std::optional<ProcessResult>& r) {
    if (!r || r->exitCode != 0) return std::nullopt;
    std::string out = r->output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.empty() || out.find('\n') != std::string::npos || out.front() != '/') return std::nullopt;
    return out;
}

// "*.obk" -> "obk"; anything else -> "".
[[maybe_unused]] std::string extensionOf(const char* pattern) {
    const std::string p = pattern ? pattern : "";
    if (p.size() > 2 && p.compare(0, 2, "*.") == 0 && p.find_first_of("*?;/ ", 2) == std::string::npos) return p.substr(2);
    return {};
}

[[maybe_unused]] std::string homeDirectory() {
    const char* home = std::getenv("HOME");
    return home && *home ? home : "/";
}

std::string ensureExtension(std::string path, const char* defaultExtension) {
    if (!defaultExtension || !*defaultExtension) return path;
    const std::string ext = std::string(".") + defaultExtension;
    const std::string name = std::filesystem::path(path).filename().string();
    if (name.find('.') == std::string::npos) path += ext;
    return path;
}

#ifdef __APPLE__

std::optional<std::string> macOpen(const char* title, FileFilter filter) {
    const std::string ext = extensionOf(filter.pattern);
    std::vector<std::string> args = {"osascript", "-e", "on run argv", "-e",
                                     ext.empty() ? "set f to choose file with prompt (item 1 of argv)"
                                                 : "set f to choose file with prompt (item 1 of argv) of type {item 2 of argv}",
                                     "-e", "return POSIX path of f", "-e", "end run", title ? title : "Open"};
    if (!ext.empty()) args.push_back(ext);
    return pathFrom(runProcess(args, true));
}

std::optional<std::string> macSave(const char* title, const std::string& suggestedName) {
    // "choose file name" asks before replacing an existing file.
    const std::vector<std::string> args = {
        "osascript", "-e", "on run argv", "-e",
        "set f to choose file name with prompt (item 1 of argv) default name (item 2 of argv)", "-e",
        "return POSIX path of f", "-e", "end run", title ? title : "Save", suggestedName.empty() ? "Untitled" : suggestedName};
    return pathFrom(runProcess(args, true));
}

#else

bool preferKde() {
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return desktop && std::strstr(desktop, "KDE") != nullptr;
}

const char* dialogTool() {
    static const char* tool = [] {
        const bool kde = inPath("kdialog");
        const bool gtk = inPath("zenity");
        if (kde && (preferKde() || !gtk)) return "kdialog";
        if (gtk) return "zenity";
        return static_cast<const char*>(nullptr);
    }();
    return tool;
}

std::optional<std::string> linuxDialog(bool save, const char* title, FileFilter filter, const std::string& suggestedPath) {
    const char* tool = dialogTool();
    if (!tool) return std::nullopt;
    std::vector<std::string> args;
    const std::string pattern = filter.pattern ? filter.pattern : "*";
    const std::string description = filter.description ? filter.description : "Files";
    if (std::strcmp(tool, "zenity") == 0) {
        args = {"zenity", "--file-selection", std::string("--title=") + (title ? title : ""),
                "--file-filter=" + description + " | " + pattern, "--file-filter=All files | *"};
        if (save) {
            args.push_back("--save");
            args.push_back("--confirm-overwrite");  // ignored (with a warning) by zenity 4, which always confirms
        }
        if (!suggestedPath.empty()) args.push_back("--filename=" + suggestedPath);
    } else {
        args = {"kdialog", "--title", title ? title : "", save ? "--getsavefilename" : "--getopenfilename",
                suggestedPath.empty() ? homeDirectory() : suggestedPath, pattern + "|" + description};
    }
    return pathFrom(runProcess(args, true));
}

#endif

}  // namespace

std::optional<std::string> openFileDialog(const char* title, FileFilter filter) {
#ifdef __APPLE__
    return macOpen(title, filter);
#else
    return linuxDialog(false, title, filter, homeDirectory() + "/");
#endif
}

std::optional<std::string> saveFileDialog(const char* title, FileFilter filter, const char* defaultExtension,
                                          const std::string& suggestedName) {
    // Suggested names come from company/customer names; keep only the file name part.
    const std::string name = std::filesystem::path(suggestedName).filename().string();
#ifdef __APPLE__
    (void)filter;
    auto path = macSave(title, name);
#else
    auto path = linuxDialog(true, title, filter, homeDirectory() + "/" + name);
#endif
    if (!path) return std::nullopt;
    return ensureExtension(*path, defaultExtension);
}

bool nativeFileDialogsAvailable() {
#ifdef __APPLE__
    return inPath("osascript");
#else
    return dialogTool() != nullptr;
#endif
}

bool openWithDefaultApp(const std::string& path) {
    // Only local, existing regular files, given as absolute paths (so they can never be
    // mistaken for an option or a URL).
    std::error_code ec;
    if (path.empty() || path.front() != '/' || !std::filesystem::is_regular_file(path, ec)) return false;
#ifdef __APPLE__
    const auto r = runProcess({"open", "--", path}, false);
#else
    if (!inPath("xdg-open")) return false;
    const auto r = runProcess({"xdg-open", path}, false);
#endif
    return r && r->exitCode == 0;
}

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
