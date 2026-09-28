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
- **Scripts** can supply the password in `OPENBOOKS_PASSWORD`. Environment variables can be
  read by other programs running as the same user, so prefer the interactive prompt, which
  doesn't echo what you type.
- The desktop app keeps only UI preferences and a recent-files list (file paths, no financial
  data) in `%APPDATA%\OpenBooks` (Windows), `~/.config/openbooks` (Linux) or
  `~/Library/Application Support/OpenBooks` (macOS). This config is not encrypted.

## Third-party code

| Component | Version | Source | SHA-256 of release archive |
|---|---|---|---|
| Dear ImGui (MIT) | v1.92.9b | https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip | `E1C46D676C2BCB7CED847BA27F50553E33A19DB97B3CADAEC7F8BE64449139F8` |

Vendored files are copied unmodified into `third_party/imgui` (core, `misc/cpp/imgui_stdlib`,
and the Win32, DX11, GLFW and OpenGL3 backends). To update, download the new release,
verify its hash, replace the files and update this table in the same commit.

## Reporting a vulnerability

Please report suspected vulnerabilities privately to the maintainers (for example through
GitHub's private vulnerability reporting on this repository) rather than in a public issue.
