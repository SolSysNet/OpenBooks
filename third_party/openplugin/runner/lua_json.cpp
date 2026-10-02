#include "lua_json.hpp"

#include "lauxlib.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace opl::runner {

namespace {

char kNullKey;       // address used as op.json.null
char kArrayMetaKey;  // registry key of the array metatable

constexpr int kMaxDepth = 64;

[[noreturn]] void fail(lua_State* L, const std::string& path, const std::string& message) {
    const std::string where = path.empty() ? "" : " at " + path;
    luaL_error(L, "cannot convert to JSON%s: %s", where.c_str(), message.c_str());
    std::abort();  // unreachable: luaL_error doesn't return
}

bool isNull(lua_State* L, int idx) { return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == &kNullKey; }

bool hasArrayMeta(lua_State* L, int idx) {
    if (!lua_getmetatable(L, idx)) return false;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kArrayMetaKey);
    const bool same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same;
}

json::Value convert(lua_State* L, int idx, int depth, const std::string& path) {
    idx = lua_absindex(L, idx);
    if (depth > kMaxDepth) fail(L, path, "nested more than 64 levels deep (is there a cycle?)");
    luaL_checkstack(L, 4, "JSON conversion");
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return json::Value();
        case LUA_TBOOLEAN: return json::Value(lua_toboolean(L, idx) != 0);
        case LUA_TNUMBER: {
            if (lua_isinteger(L, idx)) return json::Value(static_cast<long long>(lua_tointeger(L, idx)));
            const double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) fail(L, path, "numbers must be finite");
            return json::Value(d);
        }
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            std::string str(s, len);
            if (!json::isValidUtf8(str)) fail(L, path, "the string isn't valid UTF-8 (use op.base64 for binary data)");
            return json::Value(std::move(str));
        }
        case LUA_TLIGHTUSERDATA:
            if (isNull(L, idx)) return json::Value();
            fail(L, path, "unsupported value");
        case LUA_TTABLE: {
            const bool markedArray = hasArrayMeta(L, idx);
            // Classify the keys.
            lua_Integer count = 0, maxIndex = 0;
            bool allStrings = true, allPositiveInts = true;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                ++count;
                if (lua_type(L, -2) == LUA_TSTRING) {
                    allPositiveInts = false;
                } else if (lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1) {
                    allStrings = false;
                    if (lua_tointeger(L, -2) > maxIndex) maxIndex = lua_tointeger(L, -2);
                } else {
                    allStrings = allPositiveInts = false;
                }
                lua_pop(L, 1);
            }
            const bool asArray = markedArray || (count > 0 && allPositiveInts && maxIndex == count);
            if (asArray) {
                if (!allPositiveInts && count > 0) fail(L, path, "an array can only have keys 1, 2, 3, ...");
                const lua_Integer n = markedArray ? static_cast<lua_Integer>(lua_rawlen(L, idx)) : count;
                json::Array a;
                a.reserve(static_cast<std::size_t>(n));
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_rawgeti(L, idx, i);
                    a.push_back(convert(L, -1, depth + 1, path + "[" + std::to_string(i) + "]"));
                    lua_pop(L, 1);
                }
                return json::Value(std::move(a));
            }
            if (count > 0 && !allStrings) fail(L, path, "a table must have either only string keys (an object) or keys 1..n (an array)");
            json::Object o;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                std::size_t len = 0;
                const char* k = lua_tolstring(L, -2, &len);  // keys are strings: no conversion in place
                std::string key(k, len);
                if (!json::isValidUtf8(key)) fail(L, path, "a key isn't valid UTF-8");
                json::Value v = convert(L, -1, depth + 1, path.empty() ? key : path + "." + key);
                o.set(std::move(key), std::move(v));
                lua_pop(L, 1);
            }
            return json::Value(std::move(o));
        }
        default:
            fail(L, path, std::string("a ") + luaL_typename(L, idx) + " can't be converted");
    }
}

void push(lua_State* L, const json::Value& v, int depth) {
    luaL_checkstack(L, 4, "JSON conversion");
    if (depth > kMaxDepth) luaL_error(L, "JSON is nested too deeply");
    switch (v.type()) {
        case json::Type::Null: pushJsonNull(L); break;
        case json::Type::Bool: lua_pushboolean(L, v.asBool()); break;
        case json::Type::Integer: lua_pushinteger(L, static_cast<lua_Integer>(v.asInt())); break;
        case json::Type::Double: lua_pushnumber(L, v.asDouble()); break;
        case json::Type::String: lua_pushlstring(L, v.asString().data(), v.asString().size()); break;
        case json::Type::Array: {
            const json::Array& a = v.asArray();
            lua_createtable(L, static_cast<int>(a.size()), 0);
            for (std::size_t i = 0; i < a.size(); ++i) {
                push(L, a[i], depth + 1);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            pushArrayMeta(L);
            lua_setmetatable(L, -2);
            break;
        }
        case json::Type::Object: {
            const json::Object& o = v.asObject();
            lua_createtable(L, 0, static_cast<int>(o.size()));
            for (const json::Member& m : o) {
                if (m.second.isNull()) continue;
                push(L, m.second, depth + 1);
                lua_setfield(L, -2, m.first.c_str());
            }
            break;
        }
    }
}

}  // namespace

void initLuaJson(lua_State* L) {
    lua_newtable(L);
    lua_pushliteral(L, "json.array");
    lua_setfield(L, -2, "__name");
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kArrayMetaKey);
}

void pushJsonNull(lua_State* L) { lua_pushlightuserdata(L, &kNullKey); }

void pushArrayMeta(lua_State* L) { lua_rawgetp(L, LUA_REGISTRYINDEX, &kArrayMetaKey); }

void pushJson(lua_State* L, const json::Value& v) { push(L, v, 0); }

json::Value toJson(lua_State* L, int idx) { return convert(L, idx, 0, ""); }

}  // namespace opl::runner
