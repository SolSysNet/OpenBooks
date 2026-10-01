# Security

OpenBooks holds sensitive financial data. The design goal is simple: **nothing in OpenBooks
talks to the network.** Your books stay in one local file that you control.

## Network posture

- **No network code.** Neither the engine, the command line nor the desktop app opens
  sockets, makes HTTP requests, checks for updates, or sends telemetry. None of this is
  planned without a separate, opt-in design review.
- **No build-time downloads.** CMake never fetches anything (`FetchContent`,
  `ExternalProject` and package managers are not used). A build needs only a C++17
  compiler, CMake and the files in this repository. On Linux/macOS, the system's OpenSSL
  (for file encryption) and, for the desktop app, GLFW must already be installed.
  The Android app is the one exception, with pinned checksums (see [Android](#android)).
- **No URL launching.** Dear ImGui is compiled with `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`,
  which removes its built-in "open link" handler. OpenBooks only opens a local PDF it has
  just written ("Preview PDF"). It refuses anything that is not an existing local file given
  as an absolute path, so it can't open URLs or be mistaken for a command-line option.
- **No shell.** On Linux and macOS, file dialogs and "Preview PDF" run the desktop's own
  helpers (`zenity`/`kdialog`, `osascript`, `xdg-open`/`open`) through `posix_spawnp` with an
  explicit argument list. No command string is built and no shell is involved, so titles and
  file names (which can contain customer names) can't be interpreted as commands. This is
  tested with hostile names such as `$(touch …)` and `; rm -rf ~`. The helpers are looked up
  in `PATH`, the same trust boundary as launching any other desktop program.

## PDF output

Invoices and bills are rendered by OpenBooks' own small PDF writer (`src/pdf.cpp`). It has
no third-party code and no API for anything but text, lines and rectangles:

- Only the standard Helvetica fonts are referenced. No fonts are embedded or downloaded.
- No JavaScript, actions, links, forms, attachments or external references can be written.
  The test suite checks every generated PDF for these keys.
- All text (customer names, memos, ...) is converted to WinAnsi and fully escaped, so data
  can't inject PDF operators. There is a regression test for exactly this.
- Output is deterministic: no timestamps or unique ids that leak when or where a file was
  made.
- Suggested file names are sanitized, so a customer name or invoice number can't produce
  a path outside the chosen folder.

## Data at rest

- Books are a UTF-8 text file (`.obk`). Saves write a temporary file and rename it over the
  original, keeping the previous version as `.obk.bak`. Every load is checked: each
  transaction must balance and reference real accounts.
- **Password protection (optional).** `openbooks password set`, or the option in the desktop
  app, encrypts the whole file:
  - The key is derived with **PBKDF2-HMAC-SHA256** (600,000 iterations, 16-byte random salt).
  - The file is encrypted with **AES-256-GCM** (fresh 12-byte random nonce on every save).
    The header, including the iteration count, is authenticated, so any tampering or
    corruption is detected.
  - The format is documented in `include/openbooks/crypto.hpp`.
  - OpenBooks implements none of the cryptography itself. It comes from **Windows CNG** on
    Windows and the system's **OpenSSL libcrypto** on Linux and macOS (a hard build
    requirement there, never downloaded). Both backends are checked against published test
    vectors (RFC 7914 PBKDF2, McGrew–Viega AES-GCM), so files move freely between platforms.
- **What encryption protects:**
  - The temporary file and the `.bak` are encrypted too, so plaintext never reaches disk
    during a save. When a password is first added, any old unencrypted `.bak` is deleted.
  - Older unencrypted copies can still exist elsewhere: backups, cloud-sync history, or
    freed disk space. Encrypting the file doesn't reach those. Use full-disk encryption
    (BitLocker, FileVault, LUKS) as well.
  - While books are open, they and the key are in memory. The key is wiped when the books
    are closed, and passwords are wiped after use. This is best effort: memory the OS or
    the UI library copies can't be fully controlled.
  - "Preview PDF" writes an unencrypted PDF to a private temp folder. The app deletes these
    when the books are closed and when it exits. Files you export (PDF, CSV) are not
    encrypted.
- **There is no password recovery.** A forgotten password means the books can't be
  opened by anyone.
- **Scripts** can supply the password in `OPENBOOKS_PASSWORD` (read as UTF-8 on every platform,
  so non-ASCII passwords match what the interactive prompt produces). Environment variables can be
  read by other programs running as the same user, so prefer the interactive prompt, which
  doesn't echo what you type.
- The desktop app keeps only UI preferences and a recent-files list (file paths, no financial
  data) in `%APPDATA%\OpenBooks` (Windows), `~/.config/openbooks` (Linux) or
  `~/Library/Application Support/OpenBooks` (macOS). This config is not encrypted.

## Android

The Android app (`android/`) runs the same engine and reads and writes the same files.

- **No network at runtime.** The app does not request the `INTERNET` permission, so Android
  blocks every socket it could open. Sharing a PDF or CSV hands a file to an app *you* pick.
- **Build-time downloads are the exception to "no downloads".** Android apps can't
  realistically be built without Gradle, the Android Gradle Plugin, Kotlin and Jetpack
  Compose, which Gradle downloads. To keep that supply chain honest:
  - The Gradle distribution is pinned by SHA-256 in `gradle-wrapper.properties`. The committed
    `gradle-wrapper.jar` matches Gradle's published checksum.
  - `android/gradle/verification-metadata.xml` pins the SHA-256 of every artifact. A build fails
    on any mismatch or on any artifact that isn't listed.
  - Only Google's Maven (restricted to `com.android`, `com.google` and `androidx`) and Maven
    Central are allowed, and modules can't add repositories.
  - Runtime libraries are limited to Compose UI, Material 3 and activity-compose. The C++
    engine itself still has no third-party code.
- **Cryptography** comes from the platform's `javax.crypto` provider (PBKDF2WithHmacSHA256,
  AES/GCM/NoPadding), called from C++ through JNI. Before any password is used, the app runs
  the same known-answer tests as the desktop suite, plus a non-ASCII password vector. If any
  fails, it refuses to create or open password-protected books. Files interoperate with the
  Windows (CNG) and Linux/macOS (OpenSSL) builds in both directions; this is tested.
- **Data at rest:**
  - Books live in app-private storage. `allowBackup=false` and data-extraction rules keep them
    out of cloud backups and device-to-device transfers.
  - Android's file-based encryption protects that storage when the device is locked. A books
    password adds protection for exported copies, and against anyone who can read app data.
- **While password-protected books are open:**
  - The window sets `FLAG_SECURE`, which blocks screenshots, screen recording and the
    recent-apps thumbnail.
  - The books lock after 5 minutes in the background.
- **Exports:**
  - PDFs and CSVs are written to a private cache folder. They leave only through a
    `FileProvider` scoped to that folder, when you share them.
  - That folder is emptied when books are closed.
  - The request buffer that can carry a password is wiped after each call. Kotlin strings
    can't be wiped reliably, so this is best effort, as on the desktop.

## Third-party code

| Component | Version | Source | SHA-256 of release archive |
|---|---|---|---|
| Dear ImGui (MIT) | v1.92.9b | https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip | `E1C46D676C2BCB7CED847BA27F50553E33A19DB97B3CADAEC7F8BE64449139F8` |

The Android build's Gradle dependencies are not vendored. They are pinned by checksum in
`android/gradle/verification-metadata.xml` instead (see [Android](#android)).

Vendored files are copied unmodified into `third_party/imgui` (core, `misc/cpp/imgui_stdlib`,
and the Win32, DX11, GLFW and OpenGL3 backends). To update, download the new release,
verify its hash, replace the files and update this table in the same commit.

## Reporting a vulnerability

Please report suspected vulnerabilities privately to the maintainers (for example through
GitHub's private vulnerability reporting on this repository) rather than in a public issue.
