# Plugin system design

**Status:** rollout steps 1 (host core) and 2 (runner) are implemented in this repository; the app
integrations are not yet. Where building the runner settled a detail, this document was updated to
match the code.
**Applies to:** OpenBooks (`opentax/`), OpenTax (`opentax-r/`), OpenPractice (`pas-ach/`).
**Home:** this file moves to the shared `openplugin` repository once it exists.

## 1. Goals

The three apps stay complete, offline programs on their own. Plugins add optional features that the
base apps deliberately leave out, most of all features that need the network:

- OpenBooks: third-party payment portals (pay links on invoices, importing received payments).
- OpenTax: electronic filing through an authorized transmitter.
- OpenPractice: sending invoices to an accounting system.

Requirements, in priority order:

1. **The base apps stay network-free.** Every promise in each `SECURITY.md` ("no network code", "no
   build-time downloads", "no URL launching", "no shell") stays true for the app binaries. A user who
   installs no plugins gets the same program as today.
2. **Plugins can't break the books.** Every change a plugin wants goes through the same validation
   as a change typed by the user, and is all-or-nothing.
3. **The user decides what a plugin sees and where it can send it.** A plugin gets only the data its
   granted permissions cover, can reach only the network hosts it declared, and never gets the file,
   the password or the encryption key.
4. **Write once, run everywhere.** A plugin is the same files on Windows, Linux and macOS (and later
   Android), on x64 and ARM64. Plugin authors never build per platform.
5. **One implementation.** The host code is written once and shared by all three apps.

Non-goals for the first version: plugins that draw arbitrary UI, plugins inside the Android apps
(section 14), a plugin marketplace or auto-update, plugins that replace the core calculation (tax
rules, posting logic), and applying plugin changes without the user's review.

## 2. Architecture

Plugins are **Lua 5.4 scripts**. They run in **`openplugin-runner`**, a separate program that the
project builds for every platform alongside the apps. The app (the *host*) starts one runner process
per active plugin, and the two exchange JSON-RPC 2.0 messages over the runner's stdin and stdout.

```
 ┌──────────────────────── app process (no network code) ────────────────────────┐
 │                                                                                │
 │  Book / TaxReturn / Firm  ◄── adapter (per app) ──►  openplugin host library   │
 │        ▲    validation          views, proposals,     spawn, JSON-RPC, consent, │
 │        │                        extension points      plugin data, UI           │
 │   ImGui / CLI  ◄──────────── declarative UI renderer ─┘                         │
 └─────────────────────────────────────┬──────────────────────────────────────────┘
                             stdin/stdout (JSON lines)
                                        │
          ┌─────────────────────────────▼─────────────────────────────┐
          │ openplugin-runner (one process per plugin)                │
          │   sandboxed Lua 5.4: no io, os, package, debug, bytecode  │
          │   op.* API: read, propose, store, ui, json, money, http   │
          │   https only to hosts the manifest declares               │
          │   ┌────────────────────────────┐                          │
          │   │ plugin: main.lua + modules │                          │
          │   └────────────────────────────┘                          │
          └───────────────────────────────────────────────────────────┘
```

Why this shape:

- **Portable plugins.** Only the runner is compiled per platform, and the project builds it with
  the apps. A plugin is plain `.lua` text.
- **The apps stay network-free.** HTTPS lives only in the runner, a separate executable. The app
  binaries contain no socket, TLS or HTTP code, exactly as today.
- **A real sandbox.** The runner gives Lua no file system, no process and no raw sockets. A plugin
  can read only what the host sends it, and send it only to the hosts it declared. This is enforced
  by the runner, not just declared.
- **Isolation.** A plugin crash, hang or runaway loop kills its runner, not the app.
- **Readable plugins.** A user or reviewer can read exactly what a plugin does before enabling it.
- **No ABI.** Nothing about the plugin depends on the app's compiler, C++ runtime or build flags.

Why Lua: Lua 5.4 is about 30,000 lines of portable C (MIT license), easy to vendor unmodified like
Monocypher and Dear ImGui. It has been stable for decades, and it was designed to be embedded and
sandboxed. It has 64-bit integers, which keeps money math exact.

**Native plugins (advanced).** Because the host only sees JSON-RPC on a pipe, a plugin can also be
its own executable speaking the same protocol (section 4.3). This is for vendors who need a native
SDK. Native plugins are **not sandboxed** and must be built per platform. The consent screen says
both things plainly. Everything else in this document applies to them unchanged.

## 3. Code layout

A new repository, `openplugin`, vendored unmodified into each app at `third_party/openplugin/`.
CMake adds it with `add_subdirectory`, and nothing is fetched at build time.

```
openplugin/
  include/openplugin/
    json.hpp        small strict JSON reader/writer (no third-party code; size and depth limits)
    sha256.hpp      for hash pinning (checked against NIST test vectors)
    process.hpp     spawn a child with pipes: CreateProcessW on Windows, posix_spawn elsewhere
    rpc.hpp         JSON-RPC framing, request ids, timeouts, cancellation
    manifest.hpp    plugin.json parsing and validation
    registry.hpp    discovery, enable/disable, permission grants, hash pinning
    host.hpp        PluginHost: lifecycle, dispatch to the app adapter, background I/O thread
    ui.hpp          declarative UI tree (data only)
    store.hpp       per-plugin key/value data kept inside the app's own file
  src/              implementation of the above (host side; linked into the apps)
  gui/
    plugin_ui.cpp   renders ui.hpp trees with ImGui (the apps pass in their widgets and theme)
  runner/
    main.cpp        openplugin-runner: JSON-RPC loop, sandboxed lua_State, op.* API
    sandbox.cpp     allocator limit, instruction-count hook, restricted require
    http_winhttp.cpp   Windows: WinHTTP (part of Windows, like CNG)
    http_curl.cpp      Linux and macOS: the system libcurl (preinstalled on macOS)
  third_party/
    lua-5.4.x/      vendored unmodified
  docs/
    protocol.md     the protocol reference (from this document)
    lua-api.md      the op.* reference for plugin authors
  examples/
    echo/           no networking; used by the test suites
    mock-payments/  talks to a local mock server; the payments example
  tests/
```

Each app's CMake gets one new option, `<APP>_BUILD_PLUGIN_RUNNER` (default `ON`). It builds
`openplugin-runner` next to the app's executables. The host looks for the runner **only** in its own
executable's folder, never on `PATH`. Without the runner, Lua plugins are listed as "runner not
installed". A build with the option `OFF` contains no network code at all, which suits anyone
packaging a strictly offline version.

Each app adds a thin **adapter**, for example `src/plugin_adapter.cpp`, which:

- converts its model to and from JSON *views* (section 8),
- applies *proposals* through its own validated API (section 9),
- declares which extension points it supports (section 12),
- reads and writes the `PLUGIN` records in its file format (section 11).

The money, date, util, pdf and platform code that the three apps duplicate stays where it is for now.

## 4. Plugin packages

### 4.1 Layout

```
mock-payments/
  plugin.json
  main.lua
  lib/
    client.lua         loaded with require("lib.client")
  README.md
```

### 4.2 Manifest

```json
{
  "schema": 1,
  "id": "org.example.stripe-payments",
  "name": "Stripe Payments",
  "version": "1.2.0",
  "publisher": "Example Org",
  "description": "Adds pay-by-card links to invoices and imports Stripe payouts.",
  "homepage": "https://example.org/openbooks-stripe",
  "apps": { "openbooks": ">=0.6 <1.0" },
  "protocol": 1,
  "runtime": "lua",
  "main": "main.lua",
  "network": ["api.stripe.com"],
  "permissions": [
    "read:company",
    "read:contacts",
    "read:invoices",
    "propose:payments",
    "propose:transactions",
    "store"
  ],
  "extensions": ["openbooks.payments"],
  "commands": [
    { "id": "sync", "title": "Sync Stripe payments", "menu": "Banking" },
    { "id": "settings", "title": "Stripe settings...", "menu": "Plugins" }
  ]
}
```

Rules:

- `id` is reverse-DNS, `[a-z0-9.-]`, at most 64 characters. It namespaces the plugin's stored data.
- `main` and every `require`d module are paths **inside** the plugin folder (no `..`, no absolute
  paths, no symlinks leading out).
- `network` lists exact host names or one leading wildcard label (`*.example.com`). An empty or
  missing list means the plugin has no network access at all.
- Unknown keys are errors, matching how the app file formats treat unknown records.

### 4.3 Native plugins

`"runtime": "native"` replaces `main` with an argument vector per platform:

```json
"run": { "windows": ["stripe-payments.exe"], "linux": ["./stripe-payments"], "macos": ["./stripe-payments"] }
```

`run[0]` must be a file inside the plugin folder (no `PATH` lookup), so a manifest can't launch an
arbitrary system program. `network` is still required, but for native plugins it is informational
only. The consent screen shows "Not sandboxed: this plugin can read your files and use the network
freely" for every native plugin.

## 5. Installing, enabling and trust

**Discovery.** The host looks only in the per-user plugin folder:

| Platform | Folder |
|---|---|
| Windows | `%APPDATA%\<App>\plugins\` |
| Linux | `~/.config/<app>/plugins/` |
| macOS | `~/Library/Application Support/<App>/plugins/` |

It never looks in the current directory or next to an opened file, so opening a downloaded books file
can't bring a plugin with it.

**Disabled by default.** A newly found plugin appears under *Plugins > Manage plugins* as
"Not enabled". To enable it, the user reviews a consent screen showing:

- the publisher, version, folder and runtime (Lua, sandboxed; or native, not sandboxed),
- each requested permission in plain language (section 6), with high-sensitivity items marked,
- the network hosts it may contact,
- the plugin's hash, and a *Show files* button that opens the plugin's source in a read-only viewer.

**Hash pinning.** The plugin hash is SHA-256 over the sorted list of every file in the plugin folder
(each file's relative path and its own SHA-256). The registry (`plugins.json` in the app's config
folder) records, for each enabled plugin, the granted permissions, the approved network hosts and
the hash. If any file changes, the plugin is disabled until the user approves it again. A new
permission or network host in an updated version also needs approval. A user can grant fewer
permissions than requested. The plugin is told what it got and must cope with that.

There are no publisher signatures in version 1. Explicit consent plus hash pinning is the trust
model.

**Per-file opt-in.** Enabling a plugin makes it *available*. It receives data from a given books
file or return only after the user turns it on for that file (*Plugins > Use with this file*). This
choice is stored in the file's `PLUGIN` records, so a plugin enabled for business books doesn't
automatically see a personal tax return.

**The trust boundary, stated plainly.** For a Lua plugin, what it can learn is limited to the data
the user granted, and where it can send that data is limited to the hosts the user approved. Those
hosts are third parties (the payment processor, the e-file transmitter), and the consent screen
names them. A native plugin is a program running with the user's own OS privileges, and the consent
screen says so.

## 6. Permissions

| Permission | Grants | Sensitivity |
|---|---|---|
| `read:company` | Company or firm settings (name, address, fiscal year) | normal |
| `read:contacts` | Customers and vendors (OpenBooks), clients (OpenPractice) | normal |
| `read:invoices` | Invoices, credit memos and their lines | normal |
| `read:bills` | Bills and their lines | normal |
| `read:payments` | Payments and their applications | normal |
| `read:ledger` | Accounts, balances and every journal transaction | **high** |
| `read:return` | The full tax return: names, SSNs, dates of birth, every form | **high** |
| `read:result` | The computed return: every form line and diagnostics | **high** |
| `read:projects` | OpenPractice projects, phases, staff and time | normal |
| `propose:payments` | Propose received or paid payments | normal |
| `propose:transactions` | Propose manual journal transactions (for example, processor fees) | normal |
| `propose:contacts` | Propose new contacts | normal |
| `store` | Keep its own data inside the open file (section 11) | normal |
| `files` | Ask the host to show a save dialog and write a file the user chose | normal |

Network access is not a permission string. It is the manifest's `network` host list, which the
runner enforces for Lua plugins (section 7.3).

Read permissions are enforced by the host: the adapter builds only the views the grants cover.

## 7. The runner and the Lua API

### 7.1 Sandbox

Each plugin gets its own runner process and one `lua_State`:

- **Libraries:** `string` (without `string.dump`), `table`, `math`, `utf8`, `coroutine`, and the base
  library without `dofile` and `loadfile`. `collectgarbage` accepts only `"collect"`, `"count"` and
  `"step"`. `print` goes to the plugin log. `load` accepts **text chunks only**: whatever mode the
  plugin asks for, `"t"` is used. Precompiled bytecode is a known Lua sandbox escape and is never
  loaded. The `io`, `os`, `package` and `debug` libraries aren't just hidden; they aren't compiled
  into the runner at all.
- **Not available:** `io`, `os`, `package`, `debug`, `require` as shipped. `require` is replaced with
  one that loads `.lua` files from the plugin folder only, as text.
- **Time:** `op.now()` (UTC) and `op.today()` (in the user's time zone) replace `os.time`/`os.date`.
- **Memory:** a custom allocator caps the Lua heap (default 256 MiB). Exceeding it raises a Lua
  error, and the runner reports it.
- **CPU:** an instruction-count hook checks for cancellation every 1,000 instructions. Once a
  request is cancelled, the hook fires before *every* instruction, so a plugin that catches the error
  with `pcall` and loops can't get past its next instruction outside the `pcall`. The host's
  timeouts send the cancellation, and it kills the runner if a request still doesn't end.
- **Files:** at `initialize` the runner reads the whole plugin folder into memory, checks its hash
  against the one the user approved, and from then on loads `main` and every `require`d module only
  from that copy. Changing files on disk afterwards has no effect until the plugin is approved again.
- **Unwinding:** Lua is compiled as C++, so Lua errors unwind with C++ exceptions and the runner's own
  destructors still run when a plugin error passes through an `op.*` call.
- **Environment:** the runner itself starts with a scrubbed environment (section 8.1) and has nothing
  to hand the script anyway.

### 7.2 API

The runner provides one global table, `op`. A plugin registers handlers. Inside a handler, calls
like `op.read` look synchronous: the runner sends the request to the host and waits for the reply.

```lua
-- main.lua
local client = require("lib.client")

op.command("sync", function(ctx)
  local since = op.store.get("lastSync") or "2026-01-01"
  local payouts = client.listPayments(since)          -- uses op.http under the hood
  local result = op.propose{
    summary = ("Import %d Stripe payments"):format(#payouts),
    changes = client.toChanges(payouts),
    store   = { lastSync = op.today() },
  }
  if result.applied then op.ui.notify("info", "Payments imported") end
end)

op.extension("openbooks.payments", {
  createLink = function(req)
    local inv = op.read("invoice", { id = req.invoice })
    local link = client.createPaymentLink(inv)
    return { url = link.url, externalId = link.id }
  end,
  fetch = function(req) return { payments = client.listPayments(req.since) } end,
})
```

| Function | Purpose |
|---|---|
| `op.command(id, fn)` / `op.on(event, fn)` / `op.extension(name, table)` | Register handlers |
| `op.read(view, query)` | Fetch a view from the host (section 8.5); needs the matching `read:*` |
| `op.propose{ summary, changes, store }` | Propose changes (section 9); returns `{ applied, ids }` or `{ applied = false, reason }` |
| `op.store.get(key)` / `op.store.set(key, value, { secret = bool })` / `op.store.delete(key)` | Plugin data (section 11); values are text |
| `op.ui.show(tree)` / `op.ui.update(tree)` / `op.ui.close(id)` / `op.ui.on(viewId, fn)` | Declarative UI (section 10) |
| `op.ui.notify(level, message)` | Status bar message |
| `op.http.request{ method, url, headers, body, timeout }` | HTTPS request (7.3); returns `{ status, headers, body }` |
| `op.json.encode(v)` / `op.json.decode(s)` / `op.json.array(t)` / `op.json.null` | JSON (strict, same limits as the host's) |
| `op.money.add/sub/mul/cmp/parse/format` | Exact money math on `"1234.50"` strings, using integer cents |
| `op.base64`, `op.hex`, `op.sha256`, `op.hmac_sha256`, `op.random_bytes(n)` | For API signing and idempotency keys |
| `op.now()`, `op.today()` | Time |
| `op.files.save{ suggestedName, filters, content }` | Save dialog; the host writes the file |
| `op.version`, `op.app`, `op.plugin`, `op.permissions`, `op.network`, `op.locale` | Facts about this run |
| `op.progress(message, fraction)` / `op.cancelled()` | Long operations |
| `op.log(level, message)` | Plugin log |

A handler that raises a Lua error fails that one request with `PluginError`: the first line of the
message goes to the app, and the full traceback to the plugin log. The runner stays alive for the next
request. The full reference for plugin authors is [lua-api.md](lua-api.md).

### 7.3 HTTP

- **HTTPS only.** Certificates are verified against the operating system's trust store. Verification
  can't be turned off.
- **Host allowlist.** Every request, and every redirect hop, must go to a host in the approved
  `network` list. IP-literal URLs, `localhost` and non-443 ports are refused unless explicitly listed.
  That exception exists for local mock servers during plugin development, and the consent screen
  highlights it. **Plain `http://`** is allowed only for `localhost` and `127.0.0.1`, and only on a
  port the user approved; everything else must be `https://`.
- **Redirects** are followed by the runner, not the HTTP library: at most 10, each hop checked
  against the allowlist, never from `https://` to `http://`. A hop to a different host drops the
  `Authorization` and `Cookie` headers. 303 (and 301/302 after a POST) become GET, as in browsers.
- **Headers** that control the connection itself (`Host`, `Content-Length`, `Transfer-Encoding`,
  `Connection`, `Expect`, `Proxy-*`, ...) can't be set, and values can't contain line breaks.
- **No ambient credentials:** no cookie jar, no `.netrc`, and WinHTTP's automatic Windows logon is off.
- **Known limitation:** the allowlist is checked on host *names*. A host the user approved could still
  resolve to a private address (for example, a plugin author's own domain pointing at a home router).
  Checking resolved addresses is planned for the libcurl backend (`CURLOPT_OPENSOCKETFUNCTION`); WinHTTP
  has no equivalent hook.
- **Limits:** default timeout 30 s (max 300 s), max request body 16 MiB, max response 16 MiB, at most
  10 redirects. No cookie jar persists between requests.
- **No proxy settings in the plugin.** The OS proxy configuration applies.
- **Implementations:** WinHTTP on Windows (part of Windows, like CNG). The system libcurl on Linux and
  macOS (preinstalled on macOS; `libcurl4-openssl-dev` / `libcurl-devel` on Linux, needed only when
  building the runner).

The host can show a per-plugin network log (*Plugins > Show log*): method, host, path, status and
size of each request. Bodies are not logged.

## 8. Protocol

This is what flows between host and runner (or host and a native plugin). Lua plugin authors don't
need it. The runner implements it.

### 8.1 Transport

- One JSON-RPC 2.0 message per line (UTF-8, `\n`-terminated, no embedded newlines) on stdin and
  stdout. stderr is captured into the plugin log (*Plugins > Show log*), capped at 1 MiB.
- Maximum message size is 16 MiB and maximum JSON nesting depth is 64. Violating either kills the
  process.
- Both sides may send requests. Request ids are integers, unique per direction.
- The child gets a **scrubbed environment**: `PATH`, `HOME`/`USERPROFILE`, `TEMP`/`TMPDIR`, locale
  variables, and `OPENPLUGIN_PROTOCOL=1`. `OPENBOOKS_PASSWORD` and every other app variable are
  removed. The working directory is the plugin folder.
- The runner is started as `openplugin-runner --plugin <folder>`. The host passes the approved
  `network` list in `initialize`, not the manifest's list, so a grant the user narrowed is the one
  the runner enforces.
- The host does all plugin I/O on a background thread and posts results to the UI thread, so the
  ImGui frame loop never blocks. Long calls show a progress modal with *Cancel*.
- The runner handles one host request at a time (Lua is single-threaded). Requests that arrive while
  a handler runs are queued. `$/cancelRequest` is handled immediately.

### 8.2 Lifecycle

The host starts a plugin the first time it's needed for an open file, and stops it when the file is
closed or the app exits.

```
host → initialize   { protocol: 1, app: { id: "openbooks", version: "0.6.0" },
                      granted: ["read:invoices", ...], network: ["api.stripe.com"],
                      locale: "en-US", file: { opaqueId: "b3f1…", encrypted: true },
                      hash: "<the approved folder hash>", limits: { memoryBytes: 268435456 } }
plugin ← result     { protocol: 1, name: "...", version: "1.2.0",
                      commands: ["settings", "sync"], extensions: ["openbooks.payments"] }
host → initialized  (notification)
 ...
host → shutdown     plugin ← result null      (2 s grace, then the host kills the process)
```

`file.opaqueId` is a random id stored in the file. It lets a plugin tell files apart without learning
their paths.

Timeouts: `initialize` 10 s. Other requests 30 s by default; an extension method may declare a
longer limit, and a plugin may extend it with `$/progress`. On timeout the host sends
`$/cancelRequest`, then kills the process after 5 s more.

### 8.3 Host → plugin

| Method | Purpose |
|---|---|
| `command/run { id, context }` | A menu or CLI command was invoked. `context` holds the current selection, e.g. `{ "invoice": 12 }`. |
| `event/fileSaved`, `event/fileOpened` | Notifications (no reply). |
| `<extension>/<call>` | Extension point calls (section 12), with the full extension name: `openbooks.payments/createLink`. The tables in section 12 abbreviate these. |
| `ui/event { viewId, element, action, values }` | The user interacted with plugin UI (section 10). |

### 8.4 Plugin → host

| Method | Purpose | Permission |
|---|---|---|
| `host/read { view, query }` | Fetch a view (8.5) | matching `read:*` |
| `host/propose { changes, summary, store }` | Propose changes (section 9) | matching `propose:*` |
| `store/get { key }` / `store/set { key, value, secret }` / `store/delete { key }` | Plugin data (section 11) | `store` |
| `ui/show` / `ui/update` / `ui/close` | Declarative UI (section 10) | — |
| `ui/notify { level, message }` (notification) | Status bar message | — |
| `files/save { suggestedName, filters, content }` | Native save dialog; the host writes the file | `files` |
| `$/progress { token, message, fraction }` | Progress for a long call | — |
| `$/log { level, message }` | Plugin log | — |

Error codes beyond JSON-RPC's own: `-32001` PermissionDenied, `-32002` Rejected (a proposal was turned
down), `-32003` PluginError (the plugin's code raised an error), `-32004` FilesChanged (the plugin's
files don't match the approved hash), `-32800` RequestCancelled.

The host never offers "open this URL". A plugin that wants the user to visit a page shows the URL as
copyable text (section 10). This keeps the "no URL launching" guarantee.

### 8.5 Data encoding and views

- **Money:** strings with exactly two decimals, e.g. `"1234.50"`, `"-12.00"`. Never JSON numbers, so
  no language parses them as floating point. **Decimals** (rates, hours) are strings too.
- **Dates:** `"YYYY-MM-DD"`.
- **Ids:** the app's integer ids. They are stable for the life of the file.
- **Enums:** the same lowercase names the file format uses (`"invoice"`, `"received"`, ...).

Views are read-only snapshots. Example (OpenBooks):

```json
{ "view": "invoices", "query": { "status": "open", "since": "2026-01-01" } }
→ [ { "id": 12, "number": "1042", "contact": 3, "date": "2026-09-01", "due": "2026-10-01",
      "total": "1250.00", "balance": "1250.00", "currency": "USD",
      "lines": [ { "item": 4, "description": "Design work", "qty": "10", "rate": "125.00", "amount": "1250.00" } ] } ]
```

Each adapter documents its views in `docs/plugins-<app>.md`, generated from the same tables it uses
to build them.

## 9. Proposals (how plugins change data)

A plugin never writes the file. It proposes changes:

```json
{ "summary": "Import 3 Stripe payments (Sep 28 – Sep 30)",
  "changes": [
    { "op": "recordPayment", "kind": "received", "contact": 3, "date": "2026-09-30",
      "account": 1, "amount": "1250.00", "ref": "pi_3Q…", "applications": [ { "document": 12, "amount": "1250.00" } ] },
    { "op": "postTransaction", "kind": "expense", "date": "2026-09-30", "memo": "Stripe fee",
      "splits": [ { "account": 52, "debit": "36.55" }, { "account": 1, "credit": "36.55" } ] }
  ],
  "store": { "imported.pi_3Q…": "2026-09-30" } }
```

The host:

1. Checks every `op` against the plugin's `propose:*` grants.
2. Applies the changes to a **copy** of the model through the normal API (`Book::recordPayment`,
   `Book::postTransaction`, ...). Any `ob::Error` rejects the whole proposal and returns the message
   to the plugin.
3. Shows the user a review screen: summary, each change in plain language, and the effect on
   balances. Changes can be unchecked one at a time; the rest are re-validated.
4. On *Apply*: swaps in the copy, writes the `store` values, and saves. This is one atomic save, so
   the imported payments and the plugin's "already imported" markers can never get out of step.
5. Returns `{ applied: true, ids: [...] }` or `{ applied: false, reason }`.

**Every proposal is reviewed by the user.** Version 1 has no auto-apply setting.

## 10. Declarative UI

Plugins describe UI as a tree that the host renders with its own widgets and theme:

```lua
op.ui.show{
  id = "settings", title = "Stripe settings", kind = "modal",
  body = {
    { type = "text", value = "Connect a restricted API key with read access to payments." },
    { type = "field", id = "key", label = "Restricted key", input = "secret" },
    { type = "field", id = "account", label = "Deposit account", input = "account", filter = "bank" },
    { type = "copyable", label = "Dashboard", value = "https://dashboard.stripe.com/apikeys" },
    { type = "buttons", items = { { id = "save", label = "Save", primary = true }, { id = "cancel", label = "Cancel" } } },
  },
}
op.ui.on("settings", function(ev)
  if ev.element == "save" then op.store.set("apiKey", ev.values.key, { secret = true }) end
end)
```

Element types in version 1: `text`, `heading`, `separator`, `field` (input: `text`, `secret`,
`multiline`, `money`, `decimal`, `date`, `bool`, `choice`, plus app pickers such as `account`,
`contact`, `invoice`), `table`, `copyable`, `buttons`, `progress` and `diagnostics` (OpenTax's
Error/Warning/Info list, with "Go there" links).

Placement (`kind`): `modal`, `panel` (a screen under a *Plugins* sidebar section), or a slot that an
extension point defines, such as `openbooks.invoice.detail` (a box on the invoice screen).

Plugins can't supply images, fonts, HTML, markdown links or anything clickable that leaves the app.
`copyable` shows text with a *Copy* button. `secret` fields are never echoed back after the first
submit, and the host wipes them from memory once they are sent, the same as passwords.

The same tree renders in the CLI as prompts (`<app> plugin run <id> <command>`), and could later be
rendered by Jetpack Compose on Android.

## 11. Plugin data in files

Each file format gets one new record type, which every app version reads and writes even when the
plugin isn't installed:

| App | Record |
|---|---|
| OpenBooks | `PLUGIN<TAB><plugin-id><TAB><key><TAB><flags><TAB><value>` (escaped like other fields) |
| OpenTax | `PLUGIN	id=<plugin-id>	key=<key>	flags=<flags>	value=<value>` |
| OpenPractice | `PLUGIN	id=<plugin-id>	key=<key>	flags=<flags>	value=<value>` |

`flags` is empty or `secret`.

- Keys are at most 256 bytes, values at most 64 KiB, and each plugin at most 1 MiB in total.
- Records are **kept as-is** when the plugin is missing or disabled, so a file shared with someone
  without the plugin round-trips without loss. *Plugins > Remove plugin data* deletes them.
- They live in the file, so they are encrypted whenever the file is, and included in `.bak`.
- The file format version bumps (OpenBooks 1 → 2, OpenTax 1 → 2, OpenPractice 1 → 2) only if a
  `PLUGIN` record is present. Older app versions then give a clear "upgrade" error instead of
  "unknown record type".

**Secrets.** A plugin that needs an API key stores it with `op.store.set(key, value, { secret = true })`.

- In an **encrypted** file, secrets are stored like any other value, and the encryption covers them.
- In an **unencrypted** file they are still stored, with a warning. The first time a secret is
  stored, the user sees "This file isn't password-protected. The API key will be saved as readable
  text in the file and its backups. Set a password to protect it." with *Set password*, *Save anyway*
  and *Cancel*. The plugin manager keeps showing a warning badge on that plugin for that file until
  the file is encrypted.
- In every case, secret values never appear in CSV exports, PDFs, logs, `plugin data` output or the
  review screen.

## 12. Extension points

Besides generic commands and events, each app defines typed extension points. A plugin lists the ones
it implements in `extensions`.

### 12.1 OpenBooks: `openbooks.payments`

For payment portals (Stripe, Square, PayPal, ...).

| Call | Direction | Shape |
|---|---|---|
| `payments/createLink { invoice }` | host → plugin | `→ { url, externalId, expires? }`. The host stores the link under the plugin's data and shows it in the `openbooks.invoice.detail` slot as `copyable`. Optionally it is printed as **plain text** on the invoice PDF (no PDF link annotation, per the PDF policy). |
| `payments/cancelLink { invoice, externalId }` | host → plugin | Called when an invoice is voided or paid in full. |
| `payments/fetch { since }` | host → plugin | `→ { payments: [ { externalId, date, gross, fee, net, currency, invoice?, contactHint?, memo } ] }` |

The host turns `payments/fetch` results into a proposal itself: a received payment per item, applied
to the invoice when known, the fee as an expense transaction, and deduplication against `externalId`
values it already imported. Plugins therefore don't need to know OpenBooks' posting rules. The
deposit and fee accounts are chosen once in the plugin's settings and stored with `store`.

### 12.2 OpenTax: `opentax.efile`

For filing through an authorized e-file transmitter. **The plugin, not OpenTax, holds the IRS
authorization** (EFIN/ETIN or a transmitter partnership). OpenTax stays a preparation tool, and the
README says so.

| Call | Direction | Shape |
|---|---|---|
| `efile/supports { year, forms }` | host → plugin | `→ { ok, unsupported: [form ids], reason? }`. The host greys out *File electronically* with the reason. |
| `efile/preflight { return, result }` | host → plugin | `→ { diagnostics: [...] }`. Shown in Review alongside OpenTax's own diagnostics, with "Go there" links. |
| `efile/submit { return, result, signature }` | host → plugin | `→ { submissionId, status: "submitted" }`. Timeout 5 min. |
| `efile/status { submissionId }` | host → plugin | `→ { status: "submitted" \| "accepted" \| "rejected", acceptedAt?, rejects?: [ { code, message, topic } ] }` |

Rules the host enforces:

- *File electronically* is available only when OpenTax's own `Result::hasErrors()` is false and
  preflight returns no errors.
- `signature` (Self-Select PIN and prior-year AGI or PIN for each signer, plus the jurat agreement)
  is collected by an **OpenTax** screen, not plugin UI. It is sent once and never stored.
- Before sending, the user sees a confirmation naming the transmitter (the plugin's publisher and its
  network hosts), the tax year, the refund or amount owed, and that the full return including SSNs
  will be sent. They must type the taxpayer's last name to confirm.
- After acceptance, the submission id, timestamp and a hash of the submitted return are stored. If the
  return is edited afterwards, the app shows a persistent "changed since it was filed; you may need an
  amended return" banner.
- State returns would be separate extension points (`opentax.efile.state.<xx>`) once OpenTax has
  state returns.

### 12.3 OpenPractice: `openpractice.invoices`

| Call | Direction | Shape |
|---|---|---|
| `invoices/export { invoices }` | host → plugin | `→ { results: [ { invoice, externalId?, error? } ] }`. Called from *Invoices > Send to...*. |
| `invoices/fetchPayments { since }` | host → plugin | `→ payments`, turned into a proposal that updates `Invoice::paid` and status. |

This is the hook for sending OpenPractice invoices to an accounting service (QuickBooks Online, Xero,
...). Sending them into a local OpenBooks *file* doesn't fit the sandbox, since plugins have no file
access. That case is better served by an OpenBooks import command for an OpenPractice export.

### 12.4 Adding extension points

An extension point is a name, a list of methods with request and response schemas, the permissions
those imply, and the UI slots it uses. It lives in the app's adapter and in its
`docs/plugins-<app>.md`. Adding methods is backward compatible. Changing or removing them needs a new
name (`openbooks.payments2`).

## 13. Command line

```
<app> plugin list                                 # found, enabled, grants, hosts, hash status
<app> plugin enable  <id> [--grant perm,perm]     # interactive consent unless --grant covers every request
<app> plugin disable <id>
<app> plugin use     <id> <file>                  # per-file opt-in
<app> plugin run     <id> <command> <file> [--option value ...]
<app> plugin data    <id> <file> [--remove]       # show or remove the plugin's records (secrets masked)
```

Proposals on the command line print the review and ask `Apply these changes? [y/N]`. With `--yes`
they apply without asking. That is for scripts the user wrote and runs themselves, and isn't a
setting a plugin can turn on.

## 14. Android

Version 1 doesn't support plugins on Android, and the apps keep having no `INTERNET` permission. Lua
makes a later version straightforward, because the same plugin files run unchanged:

- A separate **plugins APK** embeds the runner (Lua plus the `op` API, with HTTP through Android's
  `HttpURLConnection`) and holds the `INTERNET` permission. The main app still doesn't.
- The app talks to it through a bound `Service`, using the same JSON-RPC messages.
- Consent, hash pinning and the host allowlist work as on desktop.
- The declarative UI trees render with Compose.

## 15. Versioning and compatibility

- `protocol` is an integer. A host supports a range, and `initialize` fails with a clear message if
  there's no overlap.
- Messages may gain optional fields. Both sides ignore unknown fields in *messages* (unlike files), so
  minor additions don't break older plugins.
- The `op` API version is reported as `op.version`. New functions are additive. A plugin may check
  for a function before calling it.
- `apps` in the manifest is a semver range per app. A plugin for an app version outside its range is
  shown but can't be enabled.

## 16. Decisions

| # | Question | Decision |
|---|---|---|
| 1 | Auto-apply proposals without review? | No, not in version 1. |
| 2 | Secrets in unencrypted files? | Allowed with a warning (section 11). |
| 3 | Interpreted plugins? | Lua 5.4 in a project-built, sandboxed runner. Native executables remain as an advanced, unsandboxed option. No user-chosen interpreters. |
| 4 | Signed plugins? | No. Hash pinning plus explicit consent. |
| 5 | Move shared code (money, date, pdf, ...) into openplugin? | No, leave as is. |
| 6 | OS-level sandboxing? | Later. For Lua plugins the runner already enforces network hosts and gives no file access. Native plugins remain unsandboxed. |

## 17. Rollout

1. **openplugin host core:** JSON, SHA-256, process, RPC, manifest, registry and store, with tests.
2. **openplugin-runner:** vendored Lua 5.4, sandbox, `op` API, WinHTTP and libcurl backends, the
   `echo` example. Tests include a hostile-plugin suite: sandbox escapes (`load` of bytecode,
   `string.dump`, metatable tricks against `op`), memory and CPU exhaustion, oversized messages,
   redirects to unlisted hosts, private-address URLs, and environment-variable leakage.
3. **OpenBooks:** adapter, `PLUGIN` records, *Plugins* menu and manager, consent and source viewer,
   proposal review screen, CLI commands.
4. **OpenBooks `openbooks.payments`:** with the `mock-payments` example against a local mock server.
5. **OpenTax:** adapter, `PLUGIN` records, `opentax.efile`, signature and confirmation screens, filed
   banner. Example plugin against a mock transmitter.
6. **OpenPractice:** adapter and `openpractice.invoices`.
7. **SECURITY.md** in each app gains a "Plugins" section: what the runner is, that it is the only
   component with network code, what the sandbox does and doesn't do, and how native plugins differ.
8. **Android:** later (section 14).
