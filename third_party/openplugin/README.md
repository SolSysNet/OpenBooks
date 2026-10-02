# openplugin

The shared plugin host for **OpenBooks**, **OpenTax** and **OpenPractice**. Each app vendors this
repository unmodified at `third_party/openplugin/` and adds it with `add_subdirectory`, like Dear ImGui.
Nothing is downloaded at build time.

The design is in [docs/design.md](docs/design.md). In short:

- Plugins are Lua scripts run by `openplugin-runner`, a separate, sandboxed process the project builds
  for every platform. Plugin authors never compile anything. (Native executables are an advanced,
  unsandboxed option.)
- The app and a plugin talk JSON-RPC over the plugin's stdin/stdout. The app binaries contain no
  network code.
- Plugins see only the data the user granted, change data only through proposals the user reviews,
  and must be approved again whenever any of their files change.

> Status: **steps 1 to 3 of the rollout** (design section 17): the host core, the Lua runner, and the
> OpenBooks integration (in the OpenBooks repository). Next: OpenBooks' payments extension point, then
> OpenTax and OpenPractice. Plugin authors: see [docs/lua-api.md](docs/lua-api.md).

## What's here

| Header | What it does |
|---|---|
| `json.hpp` | Strict JSON reader/writer: valid UTF-8 only, no duplicate keys, exact 64-bit integers, depth and size limits, deterministic output. |
| `sha256.hpp` | SHA-256 for hash pinning, checked against the NIST vectors. |
| `process.hpp` | Child processes with piped stdio: absolute paths only, no shell, explicit argument vectors, a scrubbed environment, only the three pipes inherited, and no orphans (a kill-on-close job object on Windows; a process group and parent-death signal on POSIX). |
| `rpc.hpp` | JSON-RPC 2.0 over those pipes. Background I/O threads; every callback runs on the thread that calls `poll()`, so app data is only touched from the UI thread. Timeouts, cancellation, answers that arrive frames later (for proposals under review), and a closed channel with a clear reason for anything malformed. |
| `manifest.hpp` | `plugin.json` parsing and validation: known permissions only, paths that can't leave the plugin folder, network host patterns, version ranges. |
| `registry.hpp` | Finds plugin folders, hashes every file in them, and records what the user approved. A changed file means the plugin needs approval again. |
| `store.hpp` | Per-plugin key/value data that lives inside the app's own file (so it's encrypted with it), with size limits, secret flags and host-reserved `@` keys. |

## openplugin-runner

`runner/` builds `openplugin-runner`, which runs one Lua plugin per process. It is the only component
with network code, and `-DOPENPLUGIN_BUILD_RUNNER=OFF` leaves it out entirely.

| File | What it does |
|---|---|
| `runner.cpp` | The protocol loop, the sandboxed Lua state (memory limit, cancellation hook, text-only `load`, `require` from the approved in-memory copy of the plugin) and the `op` API. |
| `lua_json.cpp` | Lua values to and from JSON. |
| `http.cpp` | The network policy: approved hosts, HTTPS only, redirects checked hop by hop, credentials dropped across hosts, header validation, size limits. |
| `http_winhttp.cpp` / `http_curl.cpp` | One request with no redirects: WinHTTP on Windows, the system libcurl elsewhere. |
| `codec.cpp` | Exact money math, base64, hex, HMAC-SHA256, OS random bytes. |

Lua 5.4.9 is vendored unmodified in `third_party/lua-5.4.9` (with its checksum) and compiled as C++.
Only the core and the base, coroutine, math, string, table and utf8 libraries are built.

On Linux, building the runner needs `libcurl4-openssl-dev` (Debian/Ubuntu) or `libcurl-devel` (Fedora).
macOS has libcurl already.

## Building and testing

Needs CMake 3.16+ and a C++17 compiler. On its own, the tests build by default:

```
cmake -S . -B build -G Ninja
cmake --build build
build/openplugin_tests
```

`openplugin_test_child` is a stand-in plugin that the tests drive through the real process and
JSON-RPC code. It echoes, calls back into the host, sends malformed or oversized messages, stalls,
exits and crashes on request. The runner tests start the real `openplugin-runner` on throwaway Lua
plugins (sandbox escapes, memory and CPU exhaustion, cancellation, the whole `op` API) and use a small
local HTTP server to exercise the real transport.

## License

MIT. See [LICENSE](LICENSE).
