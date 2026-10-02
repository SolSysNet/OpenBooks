#include "runner.hpp"

#include "codec.hpp"
#include "http.hpp"
#include "lua_json.hpp"

#include "openplugin/json.hpp"
#include "openplugin/manifest.hpp"
#include "openplugin/registry.hpp"
#include "openplugin/rpc.hpp"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace opl::runner {

namespace {

using rpc::RemoteError;

constexpr std::size_t kMaxLineBytes = 16u << 20;
constexpr std::size_t kDefaultMemory = 256u << 20;
constexpr int kHookInterval = 1000;  // instructions between cancellation checks

class Cancelled : public Error {
public:
    Cancelled() : Error("cancelled") {}
};

// ------------------------------------------------------------------- messages

struct Message {
    enum class Kind { Request, Notification, Response, Eof };
    Kind kind = Kind::Eof;
    std::int64_t id = 0;
    std::string method;
    json::Value params;
    bool isError = false;
    json::Value result;
    int code = 0;
    std::string message;
};

// The host is trusted, but a malformed message still shouldn't crash anything.
std::optional<Message> parseMessage(const json::Value& v) {
    if (!v.isObject()) return std::nullopt;
    Message m;
    const json::Value* id = v.find("id");
    if (const json::Value* method = v.find("method")) {
        if (!method->isString()) return std::nullopt;
        m.method = method->asString();
        if (const json::Value* p = v.find("params")) m.params = *p;
        if (id && id->isInteger()) {
            m.kind = Message::Kind::Request;
            m.id = id->asInt();
        } else {
            m.kind = Message::Kind::Notification;
        }
        return m;
    }
    if (!id || !id->isInteger()) return std::nullopt;
    m.kind = Message::Kind::Response;
    m.id = id->asInt();
    if (const json::Value* e = v.find("error")) {
        m.isError = true;
        if (const json::Value* c = e->find("code"); c && c->isInteger()) m.code = static_cast<int>(c->asInt());
        if (const json::Value* s = e->find("message"); s && s->isString()) m.message = s->asString();
    } else if (const json::Value* r = v.find("result")) {
        m.result = *r;
    }
    return m;
}

std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }

// ---------------------------------------------------------------------- Runner

class Runner;
Runner* g_runner = nullptr;  // one plugin per process

class Runner {
public:
    explicit Runner(std::filesystem::path dir) : dir_(std::move(dir)) { g_runner = this; }
    ~Runner() {
        if (L_) lua_close(L_);
        g_runner = nullptr;
    }

    int run();

    // ---- Used by the op.* functions
    json::Value callHost(const std::string& method, json::Value params);
    void notifyHost(const std::string& method, json::Value params);
    void requireRunning(const char* what) const {
        if (starting_)
            throw Error(std::string(what) + " can't be used while the plugin is loading; call it from a command or extension handler");
    }
    bool cancelled() const { return cancel_.load(); }
    HttpClient& http();
    const std::map<std::string, std::string>& files() const { return files_; }
    const Manifest& manifest() const { return manifest_; }

    std::size_t memUsed = 0;
    std::size_t memLimit = kDefaultMemory;

private:
    void readerLoop();
    Message next(bool includeDeferred);
    void send(const json::Value& v);
    void sendResult(std::int64_t id, json::Value result);
    void sendError(std::int64_t id, int code, const std::string& message);

    void handleRequest(const Message& m);
    void handleNotification(const Message& m);
    json::Value initialize(const json::Value& params);
    void setupLua();
    // Runs `body` inside lua_pcall with a traceback handler. Lua errors become RemoteError
    // (PluginError, or RequestCancelled after a cancel).
    void protect(const std::function<void(lua_State*)>& body);
    json::Value callHandler(const char* table, const std::string& key, const char* field, const json::Value& arg,
                            const std::string& notFound);

    std::filesystem::path dir_;
    std::map<std::string, std::string> files_;
    Manifest manifest_;
    json::Value app_;
    std::vector<std::string> granted_, network_;
    std::string locale_;
    lua_State* L_ = nullptr;
    bool initialized_ = false;
    bool starting_ = false;
    bool shutdown_ = false;

    std::atomic<std::int64_t> currentRequest_{-1};
    std::atomic<bool> cancel_{false};

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Message> inbox_;
    std::deque<Message> deferred_;  // host requests that arrived while a handler was running
    std::mutex outMutex_;
    std::int64_t nextId_ = 1;

    std::unique_ptr<Transport> transport_;
    std::unique_ptr<HttpClient> http_;
};

// ------------------------------------------------------------------- I/O

void Runner::readerLoop() {
    std::string buf;
    std::size_t scanned = 0;
    std::vector<char> chunk(64 * 1024);
    const auto push = [&](Message msg) {
        std::lock_guard<std::mutex> lock(m_);
        inbox_.push_back(std::move(msg));
        cv_.notify_all();
    };
    for (;;) {
        // Not fread: on a pipe it would wait for a whole buffer, not for the next message.
#ifdef _WIN32
        const int got = _read(0, chunk.data(), static_cast<unsigned>(chunk.size()));
#else
        ssize_t got;
        do {
            got = ::read(0, chunk.data(), chunk.size());
        } while (got < 0 && errno == EINTR);
#endif
        if (got <= 0) break;
        const std::size_t n = static_cast<std::size_t>(got);
        buf.append(chunk.data(), n);
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = buf.find('\n', std::max(start, scanned));
            if (nl == std::string::npos) break;
            std::string_view line(buf.data() + start, nl - start);
            start = nl + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            std::optional<Message> msg;
            try {
                json::Limits limits;
                limits.maxBytes = kMaxLineBytes;
                msg = parseMessage(json::parse(line, limits));
            } catch (const json::Error& e) {
                std::fprintf(stderr, "[runner] ignoring an invalid message from the app: %s\n", e.what());
            }
            if (!msg) continue;
            if (msg->kind == Message::Kind::Notification && msg->method == "$/cancelRequest") {
                const json::Value* id = msg->params.find("id");
                if (id && id->isInteger() && id->asInt() == currentRequest_.load()) cancel_ = true;
                continue;
            }
            push(std::move(*msg));
        }
        buf.erase(0, start);
        scanned = buf.size();
        if (buf.size() > kMaxLineBytes) {
            std::fprintf(stderr, "[runner] the app sent a message over 16 MiB; stopping\n");
            break;
        }
    }
    push(Message{});  // Eof
}

Message Runner::next(bool includeDeferred) {
    if (includeDeferred && !deferred_.empty()) {
        Message m = std::move(deferred_.front());
        deferred_.pop_front();
        return m;
    }
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [&] { return !inbox_.empty(); });
    Message m = std::move(inbox_.front());
    inbox_.pop_front();
    return m;
}

void Runner::send(const json::Value& v) {
    const std::string line = json::write(v) + "\n";
    std::lock_guard<std::mutex> lock(outMutex_);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void Runner::sendResult(std::int64_t id, json::Value result) {
    send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void Runner::sendError(std::int64_t id, int code, const std::string& message) {
    const std::string text = json::isValidUtf8(message) ? message : "error";
    send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", json::Object{{"code", code}, {"message", text}}}});
}

json::Value Runner::callHost(const std::string& method, json::Value params) {
    const std::int64_t id = nextId_++;
    send(json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
    for (;;) {
        Message m;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return !inbox_.empty(); });
            if (cancel_) throw Cancelled();
            if (inbox_.empty()) continue;
            m = std::move(inbox_.front());
            inbox_.pop_front();
        }
        switch (m.kind) {
            case Message::Kind::Eof:
                std::fflush(stdout);
                std::_Exit(0);  // the app is gone; nothing left to do
                break;
            case Message::Kind::Response:
                if (m.id != id) continue;  // stale
                if (m.isError) throw RemoteError(m.code, m.message);
                return std::move(m.result);
            default:
                deferred_.push_back(std::move(m));  // handled after the current one finishes
        }
    }
}

void Runner::notifyHost(const std::string& method, json::Value params) {
    send(json::Object{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

HttpClient& Runner::http() {
    if (!http_) {
        transport_ = makeSystemTransport();
        http_ = std::make_unique<HttpClient>(network_, *transport_);
    }
    return *http_;
}

// ------------------------------------------------------------------- main loop

int Runner::run() {
    std::thread(&Runner::readerLoop, this).detach();  // ends with the process
    while (!shutdown_) {
        Message m = next(true);
        switch (m.kind) {
            case Message::Kind::Eof: return 0;
            case Message::Kind::Response: break;  // stale answer
            case Message::Kind::Notification: handleNotification(m); break;
            case Message::Kind::Request: handleRequest(m); break;
        }
    }
    return 0;
}

void Runner::handleRequest(const Message& m) {
    currentRequest_ = m.id;
    cancel_ = false;
    try {
        json::Value result;
        if (m.method == "initialize") {
            if (initialized_) throw RemoteError(rpc::InvalidRequest, "already initialized");
            result = initialize(m.params);
        } else if (!initialized_) {
            throw RemoteError(rpc::InvalidRequest, "the plugin hasn't been initialized");
        } else if (m.method == "shutdown") {
            shutdown_ = true;
        } else if (m.method == "command/run") {
            const json::Value* id = m.params.find("id");
            if (!id || !id->isString()) throw RemoteError(rpc::InvalidParams, "command/run needs an id");
            const json::Value* ctx = m.params.find("context");
            result = callHandler("opl.commands", id->asString(), nullptr, ctx ? *ctx : json::Value(json::Object{}),
                                 "this plugin has no command '" + id->asString() + "'");
        } else if (m.method == "ui/event") {
            const json::Value* view = m.params.find("viewId");
            if (!view || !view->isString()) throw RemoteError(rpc::InvalidParams, "ui/event needs a viewId");
            result = callHandler("opl.ui", view->asString(), nullptr, m.params,
                                 "this plugin isn't handling events for '" + view->asString() + "'");
        } else {
            // Extension calls: "<extension>/<call>", e.g. "openbooks.payments/createLink".
            const std::size_t slash = m.method.rfind('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 == m.method.size())
                throw RemoteError(rpc::MethodNotFound, "unknown method " + m.method);
            const std::string ext = m.method.substr(0, slash);
            const std::string call = m.method.substr(slash + 1);
            result = callHandler("opl.extensions", ext, call.c_str(), m.params, "unknown method " + m.method);
        }
        sendResult(m.id, std::move(result));
    } catch (const RemoteError& e) {
        sendError(m.id, e.code(), e.what());
    } catch (const std::exception& e) {
        sendError(m.id, rpc::InternalError, e.what());
    }
    currentRequest_ = -1;
    cancel_ = false;
}

void Runner::handleNotification(const Message& m) {
    if (!initialized_ || m.method.rfind("event/", 0) != 0) return;
    const std::string event = m.method.substr(6);
    // Every handler for the event runs; an error in one is logged and doesn't stop the others.
    try {
        protect([&](lua_State* L) {
            lua_getfield(L, LUA_REGISTRYINDEX, "opl.events");
            if (lua_getfield(L, -1, event.c_str()) != LUA_TTABLE) return;
            const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                pushJson(L, m.params);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    std::fprintf(stderr, "[plugin] error in the %s handler: %s\n", event.c_str(), lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
        });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[plugin] error in the %s handlers: %s\n", event.c_str(), e.what());
    }
}

// ---------------------------------------------------------------- Lua plumbing

void countHook(lua_State* L, lua_Debug*) {
    if (g_runner && g_runner->cancelled()) {
        // From now on, check before every instruction: a plugin that catches the error with pcall
        // and loops can't get past its next instruction outside the pcall.
        lua_sethook(L, countHook, LUA_MASKCOUNT, 1);
        luaL_error(L, "cancelled");
    }
}

void* limitedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* r = static_cast<Runner*>(ud);
    const std::size_t old = ptr ? osize : 0;  // with ptr == NULL, osize is a type tag
    if (nsize == 0) {
        r->memUsed -= old;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > old && r->memUsed + (nsize - old) > r->memLimit) return nullptr;  // Lua reports "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (!p) return nullptr;
    r->memUsed = r->memUsed - old + nsize;
    return p;
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

struct ProtectedBody {
    const std::function<void(lua_State*)>* fn;
};

int protectedTrampoline(lua_State* L) {
    auto* body = static_cast<ProtectedBody*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    std::string err;
    try {
        (*body->fn)(L);
        return 0;
    } catch (const std::exception& e) {  // our own errors; Lua's own errors pass through untouched
        err = e.what();
    }
    return luaL_error(L, "%s", err.c_str());
}

void Runner::protect(const std::function<void(lua_State*)>& body) {
    lua_sethook(L_, countHook, LUA_MASKCOUNT, kHookInterval);
    const int base = lua_gettop(L_);
    lua_pushcfunction(L_, traceback);
    lua_pushcfunction(L_, protectedTrampoline);
    ProtectedBody pb{&body};
    lua_pushlightuserdata(L_, &pb);
    const int rc = lua_pcall(L_, 1, 0, base + 1);
    if (rc == LUA_OK) {
        lua_settop(L_, base);
        return;
    }
    std::string msg = lua_type(L_, -1) == LUA_TSTRING ? lua_tostring(L_, -1) : "error";
    lua_settop(L_, base);
    if (cancel_) throw RemoteError(rpc::RequestCancelled, "cancelled");
    if (rc == LUA_ERRMEM)
        throw RemoteError(rpc::PluginError, "the plugin ran out of memory (limit " + std::to_string(memLimit >> 20) + " MiB)");
    std::fprintf(stderr, "[plugin] %s\n", msg.c_str());
    std::fflush(stderr);
    throw RemoteError(rpc::PluginError, firstLine(msg));
}

json::Value Runner::callHandler(const char* table, const std::string& key, const char* field, const json::Value& arg,
                                const std::string& notFound) {
    json::Value out;
    bool missing = false;
    protect([&](lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, table);
        lua_getfield(L, -1, key.c_str());
        if (field) {
            if (!lua_istable(L, -1)) {
                missing = true;
                return;
            }
            lua_getfield(L, -1, field);
        }
        if (!lua_isfunction(L, -1)) {
            missing = true;
            return;
        }
        pushJson(L, arg);
        lua_call(L, 1, 1);
        out = toJson(L, -1);
    });
    if (missing) throw RemoteError(rpc::MethodNotFound, notFound);
    return out;
}

// --------------------------------------------------------------- op.* functions

Runner& R() { return *g_runner; }

// Our C++ errors (opl::Error, RemoteError, ...) become Lua errors with the caller's position;
// Lua's own errors pass straight through.
template <int (*F)(lua_State*)>
int guarded(lua_State* L) {
    std::string err;
    try {
        return F(L);
    } catch (const Cancelled&) {
        err = "cancelled";
    } catch (const std::exception& e) {
        err = e.what();
    }
    return luaL_error(L, "%s", err.c_str());
}

json::Value optJson(lua_State* L, int idx) { return lua_isnoneornil(L, idx) ? json::Value(json::Object{}) : toJson(L, idx); }

void setFunctions(lua_State* L, const luaL_Reg* fns) { luaL_setfuncs(L, fns, 0); }

// ---- registration

int op_command(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "opl.commands");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, id);
    return 0;
}

int op_on(lua_State* L) {
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "opl.events");
    if (lua_getfield(L, -1, event) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, event);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2) + 1));
    return 0;
}

int op_extension(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const auto& declared = R().manifest().extensions;
    if (std::find(declared.begin(), declared.end(), name) == declared.end())
        luaL_error(L, "extension '%s' isn't listed in plugin.json \"extensions\"", name);
    lua_getfield(L, LUA_REGISTRYINDEX, "opl.extensions");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    return 0;
}

int op_ui_on(lua_State* L) {
    const char* view = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "opl.ui");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, view);
    return 0;
}

// ---- host calls

int op_read_impl(lua_State* L) {
    R().requireRunning("op.read");
    const char* view = luaL_checkstring(L, 1);
    json::Value params = json::Object{{"view", view}, {"query", optJson(L, 2)}};
    pushJson(L, R().callHost("host/read", std::move(params)));
    return 1;
}

int op_propose_impl(lua_State* L) {
    R().requireRunning("op.propose");
    luaL_checktype(L, 1, LUA_TTABLE);
    pushJson(L, R().callHost("host/propose", toJson(L, 1)));
    return 1;
}

int op_store_get_impl(lua_State* L) {
    R().requireRunning("op.store.get");
    const char* key = luaL_checkstring(L, 1);
    const json::Value v = R().callHost("store/get", json::Object{{"key", key}});
    if (v.isString()) lua_pushlstring(L, v.asString().data(), v.asString().size());
    else lua_pushnil(L);
    return 1;
}

int op_store_set_impl(lua_State* L) {
    R().requireRunning("op.store.set");
    const char* key = luaL_checkstring(L, 1);
    std::size_t len = 0;
    const char* value = luaL_checklstring(L, 2, &len);
    bool secret = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "secret");
        secret = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    std::string v(value, len);
    if (!json::isValidUtf8(v)) luaL_argerror(L, 2, "must be valid UTF-8 text (use op.base64 for binary data)");
    R().callHost("store/set", json::Object{{"key", key}, {"value", std::move(v)}, {"secret", secret}});
    return 0;
}

int op_store_delete_impl(lua_State* L) {
    R().requireRunning("op.store.delete");
    R().callHost("store/delete", json::Object{{"key", luaL_checkstring(L, 1)}});
    return 0;
}

int op_ui_show_impl(lua_State* L) {
    R().requireRunning("op.ui.show");
    luaL_checktype(L, 1, LUA_TTABLE);
    R().callHost("ui/show", toJson(L, 1));
    return 0;
}

int op_ui_update_impl(lua_State* L) {
    R().requireRunning("op.ui.update");
    luaL_checktype(L, 1, LUA_TTABLE);
    R().callHost("ui/update", toJson(L, 1));
    return 0;
}

int op_ui_close_impl(lua_State* L) {
    R().requireRunning("op.ui.close");
    R().callHost("ui/close", json::Object{{"id", luaL_checkstring(L, 1)}});
    return 0;
}

int op_ui_notify_impl(lua_State* L) {
    const char* level = luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    R().notifyHost("ui/notify", json::Object{{"level", level}, {"message", toJson(L, 2)}});
    return 0;
}

int op_files_save_impl(lua_State* L) {
    R().requireRunning("op.files.save");
    luaL_checktype(L, 1, LUA_TTABLE);
    const json::Value v = R().callHost("files/save", toJson(L, 1));
    if (v.isString()) lua_pushlstring(L, v.asString().data(), v.asString().size());
    else lua_pushnil(L);
    return 1;
}

int op_progress_impl(lua_State* L) {
    json::Object p{{"message", toJson(L, 1)}};
    if (!lua_isnoneornil(L, 2)) p.set("fraction", luaL_checknumber(L, 2));
    R().notifyHost("$/progress", json::Value(std::move(p)));
    return 0;
}

int op_cancelled(lua_State* L) {
    lua_pushboolean(L, R().cancelled());
    return 1;
}

int op_log_impl(lua_State* L) {
    const char* level = luaL_checkstring(L, 1);
    luaL_checkstring(L, 2);
    R().notifyHost("$/log", json::Object{{"level", level}, {"message", toJson(L, 2)}});
    return 0;
}

int base_print_impl(lua_State* L) {
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        std::size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (!json::isValidUtf8(line)) line = "(print: text isn't valid UTF-8)";
    R().notifyHost("$/log", json::Object{{"level", "info"}, {"message", line}});
    return 0;
}

// ---- json

int op_json_encode(lua_State* L) {
    const std::string s = json::write(toJson(L, 1));
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int op_json_decode_impl(lua_State* L) {
    std::size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    pushJson(L, json::parse(std::string_view(s, len)));
    return 1;
}

int op_json_array(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        lua_settop(L, 0);
        lua_newtable(L);
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    pushArrayMeta(L);
    lua_setmetatable(L, 1);
    return 1;
}

// ---- money

std::int64_t checkMoney(lua_State* L, int idx) {
    const char* s = luaL_checkstring(L, idx);
    const auto cents = parseMoney(s);
    if (!cents) luaL_argerror(L, idx, lua_pushfstring(L, "'%s' isn't an amount like \"1234.50\"", s));
    return *cents;
}

void pushMoney(lua_State* L, std::int64_t cents) {
    const std::string s = formatMoney(cents);
    lua_pushlstring(L, s.data(), s.size());
}

int op_money_parse(lua_State* L) {
    const auto cents = parseMoney(luaL_checkstring(L, 1));
    if (cents) lua_pushinteger(L, *cents);
    else lua_pushnil(L);
    return 1;
}

int op_money_format(lua_State* L) {
    pushMoney(L, luaL_checkinteger(L, 1));
    return 1;
}

int op_money_add_impl(lua_State* L) {
    const int n = lua_gettop(L);
    std::int64_t total = 0;
    for (int i = 1; i <= n; ++i) total = addMoney(total, checkMoney(L, i));
    pushMoney(L, total);
    return 1;
}

int op_money_sub_impl(lua_State* L) {
    pushMoney(L, subMoney(checkMoney(L, 1), checkMoney(L, 2)));
    return 1;
}

int op_money_mul_impl(lua_State* L) {
    const std::int64_t a = checkMoney(L, 1);
    std::string factor = lua_isinteger(L, 2) ? std::to_string(lua_tointeger(L, 2)) : luaL_checkstring(L, 2);
    pushMoney(L, mulMoney(a, factor));
    return 1;
}

int op_money_cmp(lua_State* L) {
    const std::int64_t a = checkMoney(L, 1), b = checkMoney(L, 2);
    lua_pushinteger(L, a < b ? -1 : a > b ? 1 : 0);
    return 1;
}

// ---- encodings, hashing, random, time

void pushString(lua_State* L, const std::string& s) { lua_pushlstring(L, s.data(), s.size()); }

std::string checkBytes(lua_State* L, int idx) {
    std::size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return std::string(s, len);
}

int op_base64_encode(lua_State* L) {
    pushString(L, base64Encode(checkBytes(L, 1)));
    return 1;
}

int op_base64_decode(lua_State* L) {
    const auto v = base64Decode(checkBytes(L, 1));
    if (v) pushString(L, *v);
    else lua_pushnil(L);
    return 1;
}

int op_hex_encode(lua_State* L) {
    pushString(L, hexEncode(checkBytes(L, 1)));
    return 1;
}

int op_hex_decode(lua_State* L) {
    const auto v = hexDecode(checkBytes(L, 1));
    if (v) pushString(L, *v);
    else lua_pushnil(L);
    return 1;
}

int op_sha256(lua_State* L) {
    pushString(L, hexEncode(sha256Raw(checkBytes(L, 1))));
    return 1;
}

int op_hmac_sha256(lua_State* L) {
    pushString(L, hexEncode(hmacSha256Raw(checkBytes(L, 1), checkBytes(L, 2))));
    return 1;
}

int op_random_bytes_impl(lua_State* L) {
    const lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 0 && n <= 1024, 1, "must be between 0 and 1024");
    pushString(L, randomBytes(static_cast<std::size_t>(n)));
    return 1;
}

int op_now(lua_State* L) {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    lua_pushstring(L, buf);
    return 1;
}

int op_today(lua_State* L) {
    const std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    lua_pushstring(L, buf);
    return 1;
}

// ---- http

int op_http_request_impl(lua_State* L) {
    R().requireRunning("op.http.request");
    luaL_checktype(L, 1, LUA_TTABLE);
    HttpRequest req;
    if (lua_getfield(L, 1, "method") != LUA_TNIL) req.method = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "url") != LUA_TSTRING) luaL_error(L, "op.http.request needs a url");
    req.url = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "headers") == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING)
                luaL_error(L, "op.http.request headers must map names to string values");
            req.headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "body") != LUA_TNIL) req.body = checkBytes(L, -1);
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "timeout") != LUA_TNIL) {
        const lua_Number seconds = luaL_checknumber(L, -1);
        if (!(seconds > 0 && seconds <= 300)) luaL_error(L, "timeout must be between 0 and 300 seconds");
        req.timeoutMs = static_cast<int>(seconds * 1000);
        if (req.timeoutMs < 1) req.timeoutMs = 1;
    }
    lua_pop(L, 1);

    const HttpResponse resp = R().http().request(req, [] { return R().cancelled(); });

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, resp.status);
    lua_setfield(L, -2, "status");
    lua_newtable(L);
    for (const auto& [name, value] : resp.headers) {
        if (lua_getfield(L, -1, name.c_str()) == LUA_TSTRING) {  // repeated header: join like RFC 9110
            lua_pushliteral(L, ", ");
            pushString(L, value);
            lua_concat(L, 3);
        } else {
            lua_pop(L, 1);
            pushString(L, value);
        }
        lua_setfield(L, -2, name.c_str());
    }
    lua_setfield(L, -2, "headers");
    pushString(L, resp.body);
    lua_setfield(L, -2, "body");
    pushString(L, resp.url);
    lua_setfield(L, -2, "url");
    return 1;
}

// ---- sandboxed base library replacements

// load() that only accepts text: precompiled bytecode can break out of a Lua sandbox.
int base_load(lua_State* L) {
    const int n = lua_gettop(L);
    lua_settop(L, n < 4 ? 3 : 4);  // keep "env" absent unless given
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));  // the original load
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

int base_collectgarbage(lua_State* L) {
    const char* opt = luaL_optstring(L, 1, "collect");
    const std::string o = opt;
    if (o != "collect" && o != "count" && o != "step")
        luaL_argerror(L, 1, "only \"collect\", \"count\" and \"step\" are available to plugins");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// require("lib.client") loads lib/client.lua from the plugin's approved files, as text, once.
int base_require(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const std::string n = name;
    bool ok = !n.empty() && n.size() <= 128 && n.front() != '.' && n.back() != '.' && n.find("..") == std::string::npos;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) ok = false;
    if (!ok) luaL_error(L, "invalid module name '%s' (use names like \"lib.client\")", name);
    std::string path = n;
    for (char& c : path)
        if (c == '.') c = '/';
    path += ".lua";
    if (!isSafeRelativePath(path)) luaL_error(L, "invalid module name '%s'", name);

    lua_getfield(L, LUA_REGISTRYINDEX, "opl.loaded");
    if (lua_getfield(L, -1, name) != LUA_TNIL) {
        if (lua_islightuserdata(L, -1)) luaL_error(L, "module '%s' requires itself (a require loop)", name);
        return 1;
    }
    lua_pop(L, 1);
    const auto& files = R().files();
    const auto it = files.find(path);
    if (it == files.end()) luaL_error(L, "module '%s' not found (looked for %s in the plugin folder)", name, path.c_str());
    static char loading;
    lua_pushlightuserdata(L, &loading);
    lua_setfield(L, -2, name);
    const std::string chunk = "@" + path;
    if (luaL_loadbufferx(L, it->second.data(), it->second.size(), chunk.c_str(), "t") != LUA_OK) lua_error(L);
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

// ---------------------------------------------------------------- setup

void Runner::setupLua() {
    L_ = lua_newstate(limitedAlloc, this);
    if (!L_) throw Error("cannot start Lua (out of memory)");
    lua_atpanic(L_, [](lua_State* L) -> int {
        std::fprintf(stderr, "[runner] unprotected Lua error: %s\n", lua_tostring(L, -1));
        std::fflush(stderr);
        std::abort();
    });

    protect([&](lua_State* L) {
        static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine},
                                        {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                                        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
        for (const luaL_Reg& lib : libs) {
            luaL_requiref(L, lib.name, lib.func, 1);
            lua_pop(L, 1);
        }
        initLuaJson(L);
        for (const char* t : {"opl.commands", "opl.events", "opl.extensions", "opl.ui", "opl.loaded"}) {
            lua_newtable(L);
            lua_setfield(L, LUA_REGISTRYINDEX, t);
        }

        // Trim the base library.
        lua_pushglobaltable(L);
        for (const char* name : {"dofile", "loadfile"}) {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_getfield(L, -1, "load");
        lua_pushcclosure(L, base_load, 1);
        lua_setfield(L, -2, "load");
        lua_getfield(L, -1, "collectgarbage");
        lua_pushcclosure(L, base_collectgarbage, 1);
        lua_setfield(L, -2, "collectgarbage");
        lua_pushcfunction(L, guarded<base_print_impl>);
        lua_setfield(L, -2, "print");
        lua_pushcfunction(L, base_require);
        lua_setfield(L, -2, "require");
        lua_getfield(L, -1, "string");
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");  // makes bytecode
        lua_pop(L, 1);

        // op
        lua_newtable(L);
        static const luaL_Reg op[] = {
            {"command", op_command},
            {"on", op_on},
            {"extension", op_extension},
            {"read", guarded<op_read_impl>},
            {"propose", guarded<op_propose_impl>},
            {"progress", guarded<op_progress_impl>},
            {"cancelled", op_cancelled},
            {"log", guarded<op_log_impl>},
            {"sha256", op_sha256},
            {"hmac_sha256", op_hmac_sha256},
            {"random_bytes", guarded<op_random_bytes_impl>},
            {"now", op_now},
            {"today", op_today},
            {nullptr, nullptr},
        };
        setFunctions(L, op);

        const auto subtable = [&](const char* name, const luaL_Reg* fns) {
            lua_newtable(L);
            setFunctions(L, fns);
            lua_setfield(L, -2, name);
        };
        static const luaL_Reg store[] = {{"get", guarded<op_store_get_impl>},
                                         {"set", guarded<op_store_set_impl>},
                                         {"delete", guarded<op_store_delete_impl>},
                                         {nullptr, nullptr}};
        subtable("store", store);
        static const luaL_Reg ui[] = {{"show", guarded<op_ui_show_impl>},     {"update", guarded<op_ui_update_impl>},
                                      {"close", guarded<op_ui_close_impl>},   {"notify", guarded<op_ui_notify_impl>},
                                      {"on", op_ui_on},                       {nullptr, nullptr}};
        subtable("ui", ui);
        static const luaL_Reg files[] = {{"save", guarded<op_files_save_impl>}, {nullptr, nullptr}};
        subtable("files", files);
        static const luaL_Reg http[] = {{"request", guarded<op_http_request_impl>}, {nullptr, nullptr}};
        subtable("http", http);
        static const luaL_Reg jsonFns[] = {{"encode", guarded<op_json_encode>},
                                           {"decode", guarded<op_json_decode_impl>},
                                           {"array", op_json_array},
                                           {nullptr, nullptr}};
        subtable("json", jsonFns);
        lua_getfield(L, -1, "json");
        pushJsonNull(L);
        lua_setfield(L, -2, "null");
        lua_pop(L, 1);
        static const luaL_Reg money[] = {{"parse", op_money_parse},          {"format", op_money_format},
                                         {"add", guarded<op_money_add_impl>}, {"sub", guarded<op_money_sub_impl>},
                                         {"mul", guarded<op_money_mul_impl>}, {"cmp", op_money_cmp},
                                         {nullptr, nullptr}};
        subtable("money", money);
        static const luaL_Reg b64[] = {{"encode", op_base64_encode}, {"decode", op_base64_decode}, {nullptr, nullptr}};
        subtable("base64", b64);
        static const luaL_Reg hex[] = {{"encode", op_hex_encode}, {"decode", op_hex_decode}, {nullptr, nullptr}};
        subtable("hex", hex);

        // Facts about this run.
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "version");
        pushJson(L, app_);
        lua_setfield(L, -2, "app");
        pushJson(L, json::Object{{"id", manifest_.id}, {"name", manifest_.name}, {"version", manifest_.version}});
        lua_setfield(L, -2, "plugin");
        json::Array perms, hosts;
        for (const std::string& p : granted_) perms.emplace_back(p);
        for (const std::string& h : network_) hosts.emplace_back(h);
        pushJson(L, json::Value(std::move(perms)));
        lua_setfield(L, -2, "permissions");
        pushJson(L, json::Value(std::move(hosts)));
        lua_setfield(L, -2, "network");
        lua_pushstring(L, locale_.c_str());
        lua_setfield(L, -2, "locale");

        lua_setfield(L, -2, "op");  // into _G
        lua_pop(L, 1);              // _G
    });
}

json::Value Runner::initialize(const json::Value& params) {
    const json::Value* protocol = params.find("protocol");
    if (!protocol || !protocol->isInteger() || protocol->asInt() != 1)
        throw RemoteError(rpc::InvalidParams, "this runner speaks protocol 1");
    const json::Value* hash = params.find("hash");
    if (!hash || !hash->isString()) throw RemoteError(rpc::InvalidParams, "initialize needs the approved hash");

    // Read every file once, check it is exactly what the user approved, and from here on use only
    // this copy: nothing on disk is read again.
    std::vector<PluginFile> files;
    try {
        files = readPluginFolder(dir_);
    } catch (const Error& e) {
        throw RemoteError(rpc::FilesChanged, e.what());
    }
    if (hashPluginFiles(files) != hash->asString())
        throw RemoteError(rpc::FilesChanged, "the plugin's files don't match what you approved; review and approve it again");
    for (PluginFile& f : files) files_[f.path] = std::move(f.content);

    const auto manifestText = files_.find("plugin.json");
    if (manifestText == files_.end()) throw RemoteError(rpc::InvalidParams, "no plugin.json");
    manifest_ = parseManifest(manifestText->second);
    if (manifest_.runtime != Runtime::Lua) throw RemoteError(rpc::InvalidParams, "this isn't a Lua plugin");
    const auto main = files_.find(manifest_.main);
    if (main == files_.end()) throw RemoteError(rpc::InvalidParams, "the plugin's main script " + manifest_.main + " is missing");

    if (const json::Value* app = params.find("app")) app_ = *app;
    if (const json::Value* g = params.find("granted"))
        for (const json::Value& p : g->asArray()) granted_.push_back(p.asString());
    if (const json::Value* n = params.find("network"))
        for (const json::Value& h : n->asArray()) network_.push_back(h.asString());
    if (const json::Value* l = params.find("locale"); l && l->isString()) locale_ = l->asString();
    if (const json::Value* limits = params.find("limits"))
        if (const json::Value* mem = limits->find("memoryBytes"); mem && mem->isInteger())
            memLimit = static_cast<std::size_t>(std::clamp<std::int64_t>(mem->asInt(), 4 << 20, std::int64_t(4) << 30));

    setupLua();
    starting_ = true;
    try {
        protect([&](lua_State* L) {
            const std::string chunk = "@" + manifest_.main;
            if (luaL_loadbufferx(L, main->second.data(), main->second.size(), chunk.c_str(), "t") != LUA_OK) lua_error(L);
            lua_call(L, 0, 0);
        });
    } catch (...) {
        starting_ = false;
        throw;
    }
    starting_ = false;
    initialized_ = true;

    json::Array commands, extensions;
    protect([&](lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, "opl.commands");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pop(L, 1);
            commands.emplace_back(std::string(lua_tostring(L, -1)));
        }
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "opl.extensions");
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pop(L, 1);
            extensions.emplace_back(std::string(lua_tostring(L, -1)));
        }
    });
    std::sort(commands.begin(), commands.end(), [](const json::Value& a, const json::Value& b) { return a.asString() < b.asString(); });
    std::sort(extensions.begin(), extensions.end(), [](const json::Value& a, const json::Value& b) { return a.asString() < b.asString(); });
    return json::Object{{"protocol", 1},
                        {"name", manifest_.name},
                        {"version", manifest_.version},
                        {"commands", json::Value(std::move(commands))},
                        {"extensions", json::Value(std::move(extensions))}};
}

}  // namespace

int runPlugin(const std::filesystem::path& pluginDir) {
    Runner runner(pluginDir);
    return runner.run();
}

}  // namespace opl::runner
