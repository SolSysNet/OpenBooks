# Lua 5.4.9 (vendored)

Unmodified copy of `src/` from the official Lua 5.4.9 release.

- Source: https://www.lua.org/ftp/lua-5.4.9.tar.gz (released 2026-08-10, 373,429 bytes)
- SHA-256 of the tarball: `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`
  (matches the checksum published at https://www.lua.org/ftp/)
- License: MIT, see [LICENSE](LICENSE) (copied from the end of `src/lua.h`).

Only the core and the `base`, `coroutine`, `math`, `string`, `table` and `utf8` libraries are compiled
into `openplugin-runner`. `io`, `os`, `package` (`loadlib.c`), `debug`, `linit.c` and the `lua`/`luac`
programs are deliberately left out of the build; see `CMakeLists.txt` and `docs/design.md`, section 7.1.

To update: download the new release, verify its checksum against lua.org, replace `src/`, update this
file, and rerun the sandbox tests.
