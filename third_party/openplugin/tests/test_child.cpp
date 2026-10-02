// A stand-in plugin for the test suite. It speaks JSON-RPC on stdin/stdout and does whatever the
// request asks, including misbehaving:
//
//   echo {...}          -> result is the params
//   args                -> result is argv (from the OS, so it checks argument quoting)
//   getenv {name}       -> result is the variable's value, or null
//   cwd                 -> result is the working directory
//   ask {method,params} -> sends that request to the host, waits, and returns {"got": <result or error>}
//   notify {method}     -> sends a notification to the host, then returns true
//   stderr {text}       -> writes text to stderr, then returns true
//   slow {ms}           -> sleeps, then returns true
//   raw {line}          -> writes `line` verbatim (plus a newline) instead of answering
//   rawhex {hex}        -> the same, with the line given as hex (for bytes that aren't valid UTF-8)
//   huge {bytes}        -> writes one line of that many 'x's instead of answering
//   exit {code}         -> exits immediately
//   crash               -> aborts
//   stall               -> stops reading stdin and sleeps forever

#include "openplugin/json.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

using namespace opl;

namespace {

std::vector<std::string> g_args;

void sendLine(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void send(const json::Value& v) { sendLine(json::write(v)); }

void reply(const json::Value& id, json::Value result) {
    send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

bool readLine(std::string& line) { return static_cast<bool>(std::getline(std::cin, line)); }

#ifdef _WIN32
std::string narrow(const wchar_t* s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
#endif

json::Value getEnv(const std::string& name) {
#ifdef _WIN32
    std::wstring wname(name.begin(), name.end());
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return json::Value();
    std::vector<wchar_t> buf(n);
    GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    return json::Value(narrow(buf.data()));
#else
    const char* v = std::getenv(name.c_str());
    return v ? json::Value(std::string(v)) : json::Value();
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // "\n", not "\r\n"
    _setmode(_fileno(stdin), _O_BINARY);
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 0; i < wargc; ++i) g_args.push_back(narrow(wargv[i]));
    LocalFree(wargv);
#else
    for (int i = 0; i < argc; ++i) g_args.emplace_back(argv[i]);
#endif
    (void)argc;
    (void)argv;

    std::string line;
    while (readLine(line)) {
        json::Value msg = json::parse(line);
        const json::Value* method = msg.find("method");
        const json::Value* idp = msg.find("id");
        if (!method || !idp) continue;  // notifications ($/cancelRequest, ...) are ignored
        const json::Value id = *idp;
        const std::string m = method->asString();
        const json::Value params = msg.find("params") ? *msg.find("params") : json::Value();

        if (m == "echo") {
            reply(id, params);
        } else if (m == "args") {
            json::Array a;
            for (std::size_t i = 1; i < g_args.size(); ++i) a.emplace_back(g_args[i]);
            reply(id, json::Value(std::move(a)));
        } else if (m == "getenv") {
            reply(id, getEnv(params.find("name")->asString()));
        } else if (m == "cwd") {
            reply(id, json::Value(std::filesystem::current_path().u8string()));
        } else if (m == "ask") {
            json::Object req{{"jsonrpc", "2.0"}, {"id", 1000}, {"method", *params.find("method")}};
            if (const json::Value* p = params.find("params")) req.set("params", *p);
            send(json::Value(std::move(req)));
            std::string answer;
            if (!readLine(answer)) return 0;
            const json::Value response = json::parse(answer);
            const json::Value* result = response.find("result");
            reply(id, json::Object{{"got", result ? *result : *response.find("error")}});
        } else if (m == "notify") {
            send(json::Object{{"jsonrpc", "2.0"}, {"method", *params.find("method")}, {"params", json::Object{{"n", 1}}}});
            reply(id, true);
        } else if (m == "stderr") {
            const std::string& text = params.find("text")->asString();
            std::fwrite(text.data(), 1, text.size(), stderr);
            std::fflush(stderr);
            reply(id, true);
        } else if (m == "slow") {
            std::this_thread::sleep_for(std::chrono::milliseconds(params.find("ms")->asInt()));
            reply(id, true);
        } else if (m == "raw") {
            sendLine(params.find("line")->asString());
        } else if (m == "rawhex") {
            const std::string& hex = params.find("hex")->asString();
            std::string bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
            sendLine(bytes);
        } else if (m == "huge") {
            sendLine(std::string(static_cast<std::size_t>(params.find("bytes")->asInt()), 'x'));
        } else if (m == "exit") {
            std::exit(static_cast<int>(params.find("code")->asInt()));
        } else if (m == "crash") {
            std::abort();
        } else if (m == "stall") {
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
        } else {
            send(json::Object{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"error", json::Object{{"code", -32601}, {"message", "unknown method " + m}}}});
        }
    }
    return 0;
}
