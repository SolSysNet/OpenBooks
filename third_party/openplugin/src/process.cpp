#include "openplugin/process.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602  // Windows 8: nested job objects
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

namespace opl {

namespace {

bool isAllowedVariable(std::string_view name) {
#ifdef _WIN32
    static const char* const allowed[] = {"PATH", "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "TEMP", "TMP", "USERPROFILE"};
    std::string upper(name);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const char* a : allowed)
        if (upper == a) return true;
    return false;
#else
    // The proxy variables let the runner's libcurl use the user's proxy, as the OS would.
    static const char* const allowed[] = {"PATH",        "HOME",        "TMPDIR",   "LANG",    "TZ",
                                          "https_proxy", "HTTPS_PROXY", "no_proxy", "NO_PROXY"};
    for (const char* a : allowed)
        if (name == a) return true;
    return name.substr(0, 3) == "LC_";
#endif
}

}  // namespace

#ifdef _WIN32
// =================================================================== Windows

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw Error("text is not valid UTF-8");
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string lastErrorMessage() {
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text = n ? narrow(std::wstring_view(buf, n)) : "error " + std::to_string(code);
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return text;
}

// Quotes one argument so CommandLineToArgvW (and the MSVC/MinGW CRT) parse it back unchanged.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(backslashes, L'\\');
            cmd += *it;
        }
    }
    cmd += L'"';
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : h(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    HANDLE release() {
        HANDLE r = h;
        h = nullptr;
        return r;
    }
};

// A pipe whose `childEnd` is inheritable and whose parent end isn't.
void makePipe(Handle& read, Handle& write, bool childReads) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) throw Error("cannot create a pipe: " + lastErrorMessage());
    read.h = r;
    write.h = w;
    SetHandleInformation(childReads ? w : r, HANDLE_FLAG_INHERIT, 0);
}

}  // namespace

struct Process::Impl {
    Handle process;
    Handle job;
    Handle stdinWrite;
    Handle stdoutRead;
    Handle stderrRead;
    std::mutex stdinMutex;
};

Environment scrubbedEnvironment() {
    Environment env;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return env;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
        const std::wstring_view entry(p);
        if (entry[0] == L'=') continue;  // per-drive current directories ("=C:=C:\...")
        const std::size_t eq = entry.find(L'=');
        if (eq == std::wstring_view::npos) continue;
        std::string name = narrow(entry.substr(0, eq));
        if (isAllowedVariable(name)) env.emplace_back(std::move(name), narrow(entry.substr(eq + 1)));
    }
    FreeEnvironmentStringsW(block);
    return env;
}

std::unique_ptr<Process> Process::spawn(const SpawnOptions& options) {
    if (options.argv.empty()) throw Error("no program to run");
    const std::filesystem::path exe = std::filesystem::u8path(options.argv[0]);
    if (!exe.is_absolute()) throw Error("the program path must be absolute: " + options.argv[0]);
    if (options.argv[0].find('"') != std::string::npos) throw Error("the program path contains a quote");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) throw Error("program not found: " + options.argv[0]);

    std::wstring cmd = L"\"" + widen(options.argv[0]) + L"\"";
    for (std::size_t i = 1; i < options.argv.size(); ++i) {
        cmd += L' ';
        appendQuoted(cmd, widen(options.argv[i]));
    }

    // Environment block: NAME=VALUE\0 ... \0, sorted case-insensitively as Windows expects.
    std::vector<std::wstring> vars;
    for (const auto& [name, value] : options.environment) {
        if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos ||
            value.find('\0') != std::string::npos)
            throw Error("invalid environment variable '" + name + "'");
        vars.push_back(widen(name) + L"=" + widen(value));
    }
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
               CSTR_LESS_THAN;
    });
    std::wstring envBlock;
    for (const std::wstring& v : vars) {
        envBlock += v;
        envBlock += L'\0';
    }
    envBlock += L'\0';
    if (vars.empty()) envBlock += L'\0';

    auto impl = std::make_unique<Impl>();
    Handle childStdin, childStdout, childStderr;
    makePipe(childStdin, impl->stdinWrite, true);
    makePipe(impl->stdoutRead, childStdout, false);
    makePipe(impl->stderrRead, childStderr, false);

    // Only these three handles are inherited, whatever else in the app happens to be inheritable.
    HANDLE inherit[3] = {childStdin.h, childStdout.h, childStderr.h};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStorage(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize))
        throw Error("cannot start the plugin: " + lastErrorMessage());
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attrGuard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr))
        throw Error("cannot start the plugin: " + lastErrorMessage());

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childStdin.h;
    si.StartupInfo.hStdOutput = childStdout.h;
    si.StartupInfo.hStdError = childStderr.h;
    si.lpAttributeList = attrs;

    impl->job.h = CreateJobObjectW(nullptr, nullptr);
    if (!impl->job.h) throw Error("cannot start the plugin: " + lastErrorMessage());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(impl->job.h, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    const std::wstring exeW = widen(options.argv[0]);
    const std::wstring dirW = options.workingDir.wstring();
    PROCESS_INFORMATION pi{};
    // Suspended until it is in the job, so it can't start anything that escapes the job first.
    if (!CreateProcessW(exeW.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        envBlock.data(), dirW.empty() ? nullptr : dirW.c_str(), &si.StartupInfo, &pi))
        throw Error("cannot start '" + options.argv[0] + "': " + lastErrorMessage());
    Handle thread(pi.hThread);
    impl->process.h = pi.hProcess;
    if (!AssignProcessToJobObject(impl->job.h, pi.hProcess)) {
        const std::string why = lastErrorMessage();
        TerminateProcess(pi.hProcess, 1);
        throw Error("cannot start the plugin: " + why);
    }
    ResumeThread(pi.hThread);
    // childStdin/childStdout/childStderr close here, so EOF reaches whichever side outlives the other.
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}

bool Process::write(std::string_view data) {
    std::lock_guard<std::mutex> lock(impl_->stdinMutex);
    if (!impl_->stdinWrite.h) return false;
    while (!data.empty()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 20));
        if (!WriteFile(impl_->stdinWrite.h, data.data(), chunk, &written, nullptr)) return false;
        data.remove_prefix(written);
    }
    return true;
}

void Process::closeStdin() {
    std::lock_guard<std::mutex> lock(impl_->stdinMutex);
    impl_->stdinWrite.reset();
}

namespace {

std::size_t readPipe(HANDLE h, char* buf, std::size_t size) {
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buf, static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20)), &n, nullptr)) return 0;
        if (n > 0) return n;
    }
}

}  // namespace

std::size_t Process::readStdout(char* buf, std::size_t size) { return readPipe(impl_->stdoutRead.h, buf, size); }
std::size_t Process::readStderr(char* buf, std::size_t size) { return readPipe(impl_->stderrRead.h, buf, size); }

void Process::kill() {
    // Ends the plugin and anything it started; their pipe ends close, which unblocks our readers.
    TerminateJobObject(impl_->job.h, 1);
}

std::optional<int> Process::exitCode() {
    if (WaitForSingleObject(impl_->process.h, 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(impl_->process.h, &code);
    return static_cast<int>(code);
}

int Process::wait() {
    WaitForSingleObject(impl_->process.h, INFINITE);
    return *exitCode();
}

#else
// ===================================================================== POSIX

namespace {

std::string errnoMessage(int err) { return std::strerror(err); }

void closeFd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

void makePipe(int fds[2]) {
    if (::pipe(fds) != 0) throw Error("cannot create a pipe: " + errnoMessage(errno));
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

}  // namespace

struct Process::Impl {
    pid_t pid = -1;
    int stdinWrite = -1;
    int stdoutRead = -1;
    int stderrRead = -1;
    std::mutex stdinMutex;
    std::mutex waitMutex;
    bool reaped = false;
    int status = 0;

    ~Impl() {
        closeFd(stdinWrite);
        closeFd(stdoutRead);
        closeFd(stderrRead);
    }

    // Requires waitMutex.
    bool reapLocked(bool block) {
        if (reaped) return true;
        for (;;) {
            int st = 0;
            const pid_t r = ::waitpid(pid, &st, block ? 0 : WNOHANG);
            if (r == pid) {
                reaped = true;
                status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
                return true;
            }
            if (r == 0) return false;
            if (errno != EINTR) {
                reaped = true;  // not our child any more; nothing left to wait for
                status = 1;
                return true;
            }
        }
    }
};

Environment scrubbedEnvironment() {
    Environment env;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        const std::string_view name = entry.substr(0, eq);
        if (isAllowedVariable(name)) env.emplace_back(std::string(name), std::string(entry.substr(eq + 1)));
    }
    return env;
}

std::unique_ptr<Process> Process::spawn(const SpawnOptions& options) {
    static std::once_flag sigpipeOnce;
    std::call_once(sigpipeOnce, [] { std::signal(SIGPIPE, SIG_IGN); });

    if (options.argv.empty()) throw Error("no program to run");
    if (options.argv[0].empty() || options.argv[0][0] != '/')
        throw Error("the program path must be absolute: " + options.argv[0]);
    if (::access(options.argv[0].c_str(), X_OK) != 0)
        throw Error("cannot run '" + options.argv[0] + "': " + errnoMessage(errno));

    // Everything the child needs is prepared before fork(): after it, only async-signal-safe calls.
    std::vector<std::string> envStrings;
    for (const auto& [name, value] : options.environment) {
        if (name.empty() || name.find('=') != std::string::npos) throw Error("invalid environment variable '" + name + "'");
        envStrings.push_back(name + "=" + value);
    }
    std::vector<char*> argv, envp;
    for (const std::string& a : options.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (std::string& e : envStrings) envp.push_back(e.data());
    envp.push_back(nullptr);
    const std::string dir = options.workingDir.string();
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;

    int in[2], out[2], err[2], execErr[2];
    makePipe(in);
    makePipe(out);
    makePipe(err);
    makePipe(execErr);  // reports an exec failure back to us; closes by itself on success

    auto impl = std::make_unique<Impl>();
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        for (int* p : {in, out, err, execErr}) {
            ::close(p[0]);
            ::close(p[1]);
        }
        throw Error("cannot start the plugin: " + errnoMessage(e));
    }
    if (pid == 0) {
        ::setpgid(0, 0);
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        ::sigaction(SIGPIPE, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);

        ::dup2(in[0], 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        for (int fd = 3; fd < maxFd; ++fd)
            if (fd != execErr[1]) ::close(fd);
        int failure = 0;
        if (!dir.empty() && ::chdir(dir.c_str()) != 0) {
            failure = errno;
        } else {
            ::execve(argv[0], argv.data(), envp.data());
            failure = errno;
        }
        ssize_t ignored = ::write(execErr[1], &failure, sizeof failure);
        (void)ignored;
        ::_exit(127);
    }

    // Parent.
    ::setpgid(pid, pid);  // also set here, so kill() works even if we run before the child does
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    ::close(execErr[1]);
    impl->pid = pid;
    impl->stdinWrite = in[1];
    impl->stdoutRead = out[0];
    impl->stderrRead = err[0];

    int failure = 0;
    ssize_t n;
    do {
        n = ::read(execErr[0], &failure, sizeof failure);
    } while (n < 0 && errno == EINTR);
    ::close(execErr[0]);
    if (n == static_cast<ssize_t>(sizeof failure)) {
        std::lock_guard<std::mutex> lock(impl->waitMutex);
        impl->reapLocked(true);
        throw Error("cannot start '" + options.argv[0] + "': " + errnoMessage(failure));
    }
    return std::unique_ptr<Process>(new Process(std::move(impl)));
}

bool Process::write(std::string_view data) {
    std::lock_guard<std::mutex> lock(impl_->stdinMutex);
    if (impl_->stdinWrite < 0) return false;
    while (!data.empty()) {
        const ssize_t n = ::write(impl_->stdinWrite, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;  // EPIPE: the plugin closed stdin or exited
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

void Process::closeStdin() {
    std::lock_guard<std::mutex> lock(impl_->stdinMutex);
    closeFd(impl_->stdinWrite);
}

namespace {

std::size_t readFd(int fd, char* buf, std::size_t size) {
    for (;;) {
        const ssize_t n = ::read(fd, buf, size);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno != EINTR) return 0;
    }
}

}  // namespace

std::size_t Process::readStdout(char* buf, std::size_t size) { return readFd(impl_->stdoutRead, buf, size); }
std::size_t Process::readStderr(char* buf, std::size_t size) { return readFd(impl_->stderrRead, buf, size); }

void Process::kill() {
    std::lock_guard<std::mutex> lock(impl_->waitMutex);
    if (impl_->reaped) return;  // the pid may already belong to someone else
    ::kill(-impl_->pid, SIGKILL);
    ::kill(impl_->pid, SIGKILL);
}

std::optional<int> Process::exitCode() {
    std::lock_guard<std::mutex> lock(impl_->waitMutex);
    if (!impl_->reapLocked(false)) return std::nullopt;
    return impl_->status;
}

int Process::wait() {
    // Polls rather than blocking in waitpid while holding the lock, so kill() stays usable.
    for (;;) {
        if (auto code = exitCode()) return *code;
        ::usleep(5000);
    }
}

#endif

// ================================================================== shared

Process::Process(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Process::~Process() {
    if (!exitCode()) {
        kill();
        wait();
    }
}

}  // namespace opl
