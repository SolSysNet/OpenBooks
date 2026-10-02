#pragma once

// A child process with its stdin, stdout and stderr connected to pipes.
//
// Hardening, because the child is a plugin:
//   - argv[0] must be an absolute path. There is no PATH search and no shell; arguments are passed
//     as a vector (on Windows, quoted by the CommandLineToArgvW rules).
//   - The child receives exactly the environment given, never the parent's (see
//     scrubbedEnvironment()).
//   - The child inherits only its three pipe ends, no other handles or file descriptors.
//   - The child can't outlive the app: on Windows it runs in a kill-on-close job object; on POSIX
//     it gets its own process group (killed as a group), and on Linux a parent-death signal.
//
// Thread safety: one thread may write while others read stdout and stderr; kill() may be called
// from any thread and unblocks the readers.
//
// POSIX note: the first spawn sets SIGPIPE to ignored for the whole process, so writing to a
// plugin that has exited returns an error instead of killing the app.

#include "openplugin/error.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opl {

using Environment = std::vector<std::pair<std::string, std::string>>;

struct SpawnOptions {
    std::vector<std::string> argv;       // UTF-8; argv[0] is the absolute path of the executable
    std::filesystem::path workingDir;    // must exist
    Environment environment;             // the child's complete environment
};

class Process {
public:
    // Starts the child. Throws opl::Error if it can't be started (including a missing or
    // non-executable argv[0]).
    static std::unique_ptr<Process> spawn(const SpawnOptions& options);

    ~Process();  // kills the child if it is still running
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Writes all of `data` to the child's stdin. False if the child closed it (or exited).
    bool write(std::string_view data);
    void closeStdin();

    // Block until some output is available. Return the byte count, or 0 at end of stream.
    std::size_t readStdout(char* buf, std::size_t size);
    std::size_t readStderr(char* buf, std::size_t size);

    void kill();
    // The exit code once the child has exited, without waiting.
    std::optional<int> exitCode();
    int wait();

    struct Impl;

private:
    explicit Process(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The environment for a plugin: only the variables a well-behaved program needs to find temp
// folders, the user's locale and the system (on Windows, SystemRoot is required by the OS itself).
// Everything else, including OPENBOOKS_PASSWORD and other app settings, is left out.
//   POSIX:   PATH HOME TMPDIR LANG LC_* TZ https_proxy HTTPS_PROXY no_proxy NO_PROXY
//   Windows: PATH SystemRoot SystemDrive windir TEMP TMP USERPROFILE
Environment scrubbedEnvironment();

}  // namespace opl
