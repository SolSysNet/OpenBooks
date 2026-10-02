# Writing a Lua plugin

Plugins for OpenBooks, OpenTax and OpenPractice are Lua 5.4 scripts. The app runs them in
`openplugin-runner`, a sandbox with no file, process or socket access. Everything a plugin does goes
through one global table, `op`. The design and the reasons for it are in [design.md](design.md).

## A minimal plugin

```
hello/
  plugin.json
  main.lua
```

```json
{
  "schema": 1,
  "id": "org.example.hello",
  "name": "Hello",
  "version": "1.0.0",
  "publisher": "Example",
  "apps": { "openbooks": ">=0.6" },
  "protocol": 1,
  "runtime": "lua",
  "main": "main.lua",
  "permissions": ["read:invoices"],
  "commands": [ { "id": "count", "title": "Count open invoices" } ]
}
```

```lua
op.command("count", function(ctx)
  local invoices = op.read("invoices", { status = "open" })
  op.ui.notify("info", ("%d open invoices"):format(#invoices))
end)
```

Put the folder in the app's plugin folder, then enable it under *Plugins > Manage plugins*. Any change
to any file in the folder means the user has to approve the plugin again.

## How a plugin runs

- `main.lua` runs once when the plugin starts. It registers handlers. It can't call the app or the
  network yet; those calls raise "can't be used while the plugin is loading".
- After that, the app calls the handlers: commands from menus, extension calls, UI events and file
  events. One handler runs at a time; calls into the app (`op.read`, ...) look synchronous.
- A handler's return value goes back to the app as JSON. An error fails just that call: the first
  line of the message is shown to the user and the traceback goes to the plugin log
  (*Plugins > Show log*).
- The user (or a timeout) can cancel a call. `op.cancelled()` tells a long loop to stop early; if it
  doesn't, the runner stops it.
- Memory is limited (256 MiB by default). Running out fails the call, not the plugin.

## The sandbox

Available: `string` (no `string.dump`), `table`, `math`, `utf8`, `coroutine`, and the base library
except `dofile` and `loadfile`. `load` compiles **text only**, never bytecode. `collectgarbage` accepts
`"collect"`, `"count"` and `"step"`. `print` writes to the plugin log.

Not available: `io`, `os`, `package`, `debug`. Use `op.now()`/`op.today()` for time, `op.http` for
the network, `op.store` for anything you need to keep, and `op.files.save` to give the user a file.

`require("lib.client")` loads `lib/client.lua` from the plugin folder (as text, once, like standard
Lua). Module names are letters, digits, `_`, `-` and dots.

## Reference

### Handlers

| Function | |
|---|---|
| `op.command(id, fn)` | `fn(context)` runs when the user picks the command (declared in `plugin.json` `commands`). `context` holds the selection, e.g. `{ invoice = 12 }`. |
| `op.extension(name, handlers)` | Implements an extension point listed in `plugin.json` `extensions`: `handlers` maps call names to functions, e.g. `{ createLink = function(req) ... end }`. |
| `op.on(event, fn)` | `fn(details)` for app events: `"fileSaved"`, `"fileOpened"`. Several handlers may listen; an error in one doesn't stop the others. |
| `op.ui.on(viewId, fn)` | `fn(event)` when the user interacts with a view you showed: `event.element`, `event.action`, `event.values`. |

### The app

| Function | Permission | |
|---|---|---|
| `op.read(view, query)` | `read:*` for that view | A read-only snapshot, e.g. `op.read("invoices", { status = "open" })`. Each app documents its views. |
| `op.propose{ summary, changes, store }` | `propose:*` | Suggests changes. The user reviews them. Returns `{ applied = true, ids = {...} }` or `{ applied = false, reason = "..." }`. |
| `op.store.get(key)` | `store` | Your saved text value, or `nil`. |
| `op.store.set(key, value, { secret = true })` | `store` | Saves text inside the user's file. Mark API keys `secret`. |
| `op.store.delete(key)` | `store` | |
| `op.files.save{ suggestedName, filters, content }` | `files` | Shows a save dialog. Returns the chosen path or `nil`. |

### UI

| Function | |
|---|---|
| `op.ui.show(view)` / `op.ui.update(view)` / `op.ui.close(id)` | Shows a declarative view (design.md, section 10). |
| `op.ui.notify(level, message)` | A status bar message. `level` is `"info"`, `"warn"` or `"error"`. |
| `op.progress(message, fraction)` | Progress for the current call; `fraction` is 0-1 or `nil`. |
| `op.log(level, message)` | The plugin log. |

### Network

```lua
local r = op.http.request{
  method = "POST",                       -- GET (default), HEAD, POST, PUT, PATCH, DELETE
  url = "https://api.example.com/v1/charges",
  headers = { Authorization = "Bearer " .. op.store.get("apiKey"), ["Content-Type"] = "application/json" },
  body = op.json.encode({ amount = 1250 }),
  timeout = 30,                          -- seconds, up to 300
}
-- r.status, r.headers (lowercase names), r.body, r.url (after redirects)
```

Only the hosts in `plugin.json` `network` (and approved by the user) can be reached, over HTTPS.
Redirects are followed and checked hop by hop; credentials are dropped when a redirect leaves the
host. For a local test server, list it with its port (`"localhost:8080"`); plain `http://` works only
for `localhost` and `127.0.0.1`. Bodies are limited to 16 MiB each way. Errors (unreachable host,
timeout, a refused redirect) raise Lua errors; HTTP error statuses (404, 500, ...) are returned
normally.

### JSON

| Function | |
|---|---|
| `op.json.encode(value)` | A table with keys 1..n is an array, one with string keys an object; mixing them is an error. Strings must be valid UTF-8. |
| `op.json.decode(text)` | Strict JSON. Integers stay integers. `null` inside an array becomes `op.json.null`; `null` object members are left out. |
| `op.json.array(t)` | Marks `t` (or a new table) as an array, so an empty one encodes as `[]`. |
| `op.json.null` | Encodes as `null`. |

Everything passed to or from the app goes through the same conversion.

### Money

Amounts are strings with two decimals, like `"1234.50"`, computed in exact integer cents (never
floating point). Rounding is half away from zero, the same as the apps.

| Function | |
|---|---|
| `op.money.add(a, b, ...)` / `op.money.sub(a, b)` | `op.money.add("0.10", "0.20")` is `"0.30"`. |
| `op.money.mul(amount, factor)` | `factor` is a decimal string or an integer: `op.money.mul("10.00", "0.0725")` is `"0.73"`. |
| `op.money.cmp(a, b)` | -1, 0 or 1. |
| `op.money.parse(text)` / `op.money.format(cents)` | To and from integer cents. `parse` returns `nil` for anything that isn't an amount. |

### Encoding, hashing and time

| Function | |
|---|---|
| `op.base64.encode(s)` / `op.base64.decode(s)` | Standard alphabet with padding. `decode` returns `nil` if invalid. |
| `op.hex.encode(s)` / `op.hex.decode(s)` | |
| `op.sha256(s)` / `op.hmac_sha256(key, message)` | Lowercase hex, for API request signing. |
| `op.random_bytes(n)` | Up to 1024 bytes from the OS's secure random source (idempotency keys, nonces). |
| `op.now()` | UTC, `"2026-10-01T23:45:43Z"`. |
| `op.today()` | The user's local date, `"2026-10-01"`. |
| `op.cancelled()` | True once the current call has been cancelled. |

### Facts

`op.version` (the API version, 1), `op.app` (`{ id, version }`), `op.plugin` (`{ id, name, version }`),
`op.permissions` and `op.network` (what the user actually granted, which may be less than you asked
for), `op.locale`.
