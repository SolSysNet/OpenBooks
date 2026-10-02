# Contributing to OpenBooks

Thanks for helping. OpenBooks holds people's financial records, so this project cares more about
**correct**, **safe** and **boring** than clever. These guidelines explain what that means in
practice.

## Ground rules

These four rules are not negotiable. A change that breaks one won't be merged, however useful it is.

1. **The books always balance.** Every transaction's debits equal its credits, the Balance Sheet
   balances, and the sub-ledgers (open invoices, bills and credits) agree with A/R and A/P.
2. **Money is never floating point.** Use `ob::Money` (integer cents) and `ob::Decimal`
   (quantities and rates). Rounding happens in one place, `multiply()` / `percentOf()`.
3. **Nothing talks to the network.** No sockets, HTTP, update checks or telemetry in the engine,
   the CLI or the desktop app, and no build-time downloads (`FetchContent`, `ExternalProject`,
   package managers). See [SECURITY.md](SECURITY.md). Features that need the network belong in a
   plugin (see [Plugins](README.md#plugins)); the only network code is `openplugin-runner` in
   `third_party/openplugin`, and changes there start as a design discussion in an issue. The one exception is the Android build, which downloads
   checksum-pinned Gradle dependencies (see [Android app guidelines](#android-app-guidelines-android)).
4. **History is kept.** Records are voided, never deleted, and closed periods stay closed.

## Getting started

You need CMake 3.16+ and a C++17 compiler (GCC 9+, Clang 10+ or MSVC 2019+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

- On Linux and macOS, OpenSSL's libcrypto must be installed (`libssl-dev`, `openssl-devel` or
  `brew install openssl@3`). It provides the file encryption; Windows uses its built-in CNG.
- The desktop app builds by default. On Linux install GLFW (`libglfw3-dev`); on macOS use
  `brew install glfw`. Use `-DOPENBOOKS_BUILD_GUI=OFF` to build only the engine and CLI.
- Do GUI work in a **Debug** build: Dear ImGui's assertions catch real layout bugs there that a
  Release build silently hides.
- Never test with real books. Create a throwaway file:
  `openbooks -f scratch.obk init --company "Test Co"`.

See the README's *Project layout* section for a map of the code.

## Making a change

1. **Open an issue first** for anything larger than a small fix, so we can agree on the
   approach before you write it. Accounting behavior, the file format, dependencies and
   anything security-related always need an issue.
2. **Branch from `main`**, named `feature/<topic>` or `fix/<topic>`.
3. **Keep it focused.** One logical change per pull request, split into commits that each build
   and pass the tests.
4. **Add tests** (see [Testing](#testing)) and update the README, CLI `help` text and
   SECURITY.md when behavior they describe changes.
5. **Open a pull request** using the checklist below.

### Commit messages

- The subject line is imperative, 72 characters or fewer, with no trailing period ("Add credit memos",
  "Fix startup crash from corrupted config").
- The body explains **why**, and what a reviewer can't see from the diff: the bug's root cause,
  alternatives you rejected, and compatibility notes.

## Engine guidelines (`src/`, `include/openbooks/`)

- **Validate first, then change state.** Every `Book` mutation checks everything it can, and only
  then modifies anything, so a thrown `ob::Error` leaves the book untouched. Operations that span
  several steps work on a copy (`Book draft = *this; …; *this = std::move(draft);`), as
  `createDueRecurringInvoices()` does.
- **Errors are user-facing.** Throw `ob::Error` with a message a bookkeeper understands:
  lowercase, no trailing period, naming the record ("invoice 1003 only has 49.00 left to pay").
- **Posting rules live in one place.** How a document hits the ledger is decided in
  `Book::createDocument()`. Don't post invoice, bill or payment entries anywhere else. A/R and A/P
  must only change through documents and payments.
- **The engine has no UI.** `openbooks_core` must not depend on the CLI or GUI. Anything both need
  (like CSV import or PDF output) belongs in the core.
- **Keep dependencies minimal.** The engine uses only the C++17 standard library, plus the
  platform crypto provider for file encryption (CNG on Windows, OpenSSL elsewhere; see
  `src/crypto_*.cpp`). Never implement cryptography yourself; use those primitives.

### Changing the file format

Books files are plain text (`src/storage.cpp` documents the layout). People keep years of records
in them, so:

- **New releases must read every older file.** Add new fields at the *end* of a record and read
  them with `Fields::optional()` / `optionalInt()` using sensible defaults.
- **Bump `kFileVersion`** whenever an older release would *misread* the new file (for example a
  new record type or a new enum value). An older release then refuses the file with "upgrade
  OpenBooks" instead of silently losing data.
- **Test both directions:** a write → read → write round trip must produce identical text, and
  a hand-written file in the previous format must still load (see
  `company_fields_round_trip_and_old_files_load`).

## Desktop app guidelines (`gui/`)

- **All changes go through `App::commit()`.** It applies the change to a copy, saves, and only
  then swaps the copy in.
- **Never hold references into the `Book` across a `commit()`.** Commit replaces the books, so
  references and iterators dangle. Remember ids, copy what you need (`const Document doc =
  b.document(id);`), and when a button inside a loop would commit, record the action and run it
  after the loop (see the `deferred` pattern in `app_documents.cpp`).
- **Pass by value when the source might be modified.** `openBooks(recent_.front())` once passed a
  reference into the very list being reordered; that use-after-free corrupted configs and crashed
  the app. When in doubt, copy.
- **Treat files on disk as untrusted.** Validate config and user files when loading, and don't
  let a bad value reach a function that throws.
- **Platform code stays in `platform_*.cpp`.** Launch external programs only through the
  `posix_spawnp` helper with an explicit argument list: never `system()`, `popen()` or a shell.
  Only open local files the app wrote itself, as absolute paths.
- **Match the look.** Use the `ui::` widgets and theme colors (`colorAccent()`,
  `ui::statusColor()` …) rather than hard-coded colors, and check light *and* dark themes.

## Android app guidelines (`android/`)

- The app is a thin UI. Accounting rules belong in the engine; if the app needs something new,
  add a JSON operation to `app/src/main/cpp/jni_bridge.cpp` that calls the engine.
- Money crosses the bridge as decimal **strings** (the bridge rejects JSON floats). In Kotlin,
  use `MoneyFmt` and `BigDecimal` for display and previews, never `Double`.
- Never add the `INTERNET` permission, or anything that asks for it.
- Dependencies are pinned. After changing `gradle/libs.versions.toml`, run
  `./gradlew --write-verification-metadata sha256 help assembleDebug assembleRelease` and commit
  the reviewed `verification-metadata.xml` diff with the change. New libraries need an issue
  first, like any other dependency.
- `./gradlew assembleDebug assembleRelease` must build without Kotlin or C++ warnings.
- Test bridge changes on a desktop JVM (see android/README.md), then on an emulator.

## Code style

Match the surrounding code. Specifically:

- C++17, 4-space indent, braces on the same line, lines up to about 120 columns.
- `CamelCase` types, `camelCase` functions and variables, `member_` for private members,
  `kConstant` for constants. Engine code is in namespace `ob`, GUI code in `obgui`.
- Comments explain *why* (a rule, a trade-off, a trap), not what the next line does.
- **No new warnings.** The project builds cleanly with `-Wall -Wextra -Wpedantic` (and `/W4` on
  MSVC), and it should stay that way.
- Prefer small free functions in an anonymous namespace over new classes.

## Testing

Tests live in `tests/test_main.cpp` and use a tiny built-in framework (`TEST`, `CHECK`,
`CHECK_EQ`, `CHECK_THROWS`). No test framework dependency is needed.

- **Accounting changes need tests** that check the resulting balances, not just that nothing
  threw. Assert the affected accounts, sub-ledger balances and, where it matters, that the
  Balance Sheet still balances or that `sumOfAllBalances()` is zero.
- **Test the refusals too:** invalid input, closed periods, void rules, "all or nothing" rollback.
- **PDF output** must pass `pdfStructureValid()` and `noActiveContent()`.
- **Bug fixes** come with a test that fails without the fix whenever the bug is testable in the
  engine or CLI.

The GUI has no automated tests yet, so describe how you checked a GUI change in the PR. At a
minimum:

- Exercise it in a Debug build.
- Check both themes.
- Start the app **both with and without** a file argument. Startup with no argument reopens the
  most recent file, and that path has hidden bugs before.

Linux-specific code can be compiled and checked on Windows using WSL.

## Pull request checklist

Copy this into your PR description:

```markdown
## What and why

## How I tested it
- [ ] `ctest` passes (N checks)
- [ ] Builds with no new warnings (Debug and Release)
- [ ] GUI: Debug build, light + dark theme, started with and without a file (if applicable)

## Checklist
- [ ] Accounting changes keep the books balanced and have tests checking the balances
- [ ] No network access, no new dependencies, no build-time downloads (Android: pinned in `verification-metadata.xml`)
- [ ] File format: older files still load; `kFileVersion` bumped if older releases would misread it
- [ ] README / CLI help / SECURITY.md updated where behavior changed
```

## Dependencies

New third-party code needs a strong reason and an issue first. If it's accepted:

- Vendor it into `third_party/` from an official release, unmodified.
- Record its version, source URL and the release archive's **SHA-256** in the table in
  SECURITY.md, in the same commit.
- Its license must be compatible with MIT.

## Reporting bugs

Open an issue with:

- your OS and OpenBooks version (`openbooks version`),
- what you did, what you expected, and what happened,
- a minimal books file that shows it, if you can share one.

**Never attach your real books:** they contain financial data. Recreate the problem in a scratch
file instead.

**Security vulnerabilities** must not be reported in public issues. Follow
[SECURITY.md](SECURITY.md).

## Conduct

Be respectful and assume good faith. Critique code, not people. We want contributing to be
pleasant for bookkeepers and programmers alike.

## License

OpenBooks is MIT-licensed. By submitting a contribution you agree that it's licensed under the
[MIT License](LICENSE) and that you have the right to submit it.
