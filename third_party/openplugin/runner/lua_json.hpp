#pragma once

// Converting between Lua values and JSON.
//
//   Lua -> JSON: nil and op.json.null become null; integers stay integers; floats must be finite;
//   strings must be valid UTF-8 (binary data goes through op.base64 or op.hex). A table is an array
//   if it was marked with op.json.array() or its keys are exactly 1..n, an object if every key is a
//   string, and an error otherwise. An empty table is an object unless marked as an array.
//
//   JSON -> Lua: objects and arrays become tables (arrays are marked, so they round-trip even when
//   empty). null members of objects are left out, so `if t.memo then` works; nulls inside arrays
//   become op.json.null so positions are kept.
//
// Lua is compiled as C++ here, so these may raise Lua errors (C++ exceptions) and destructors still
// run.

#include "openplugin/json.hpp"

#include "lua.h"

namespace opl::runner {

// Registers the array metatable and the null sentinel. Call once per lua_State.
void initLuaJson(lua_State* L);
// Pushes op.json.null (a light userdata) and the array metatable.
void pushJsonNull(lua_State* L);
void pushArrayMeta(lua_State* L);

void pushJson(lua_State* L, const json::Value& v);
// Raises a Lua error describing the problem (with a path such as "lines[2].amount").
json::Value toJson(lua_State* L, int idx);

}  // namespace opl::runner
