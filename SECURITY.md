# Security

OpenBooks holds sensitive financial data. The design goal is simple: **nothing in OpenBooks
talks to the network.** Your books stay in one local file that you control.

## Network posture

- **No network code.** Neither the engine, the command line nor the desktop app opens
  sockets, makes HTTP requests, checks for updates, or sends telemetry. None of this is
  planned without a separate, opt-in design review.
- **No build-time downloads.** CMake never fetches anything (`FetchContent`,
  `ExternalProject` and package managers are not used). A build needs only a C++17
  compiler, CMake and the files in this repository. On Linux/macOS the desktop app also
  needs the system GLFW package.
- **No URL launching.** Dear ImGui is compiled with `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`,
  which removes its built-in "open link" handler. OpenBooks' only shell call opens a local
  PDF it has just written (Windows "Preview PDF"). It refuses anything that is not an
  existing local file, so it can't open URLs.

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

- Books are a plain UTF-8 text file (`.obk`). Saves write a temporary file and rename it
  over the original, keeping the previous version as `.obk.bak`. Every load is checked:
  each transaction must balance and reference real accounts.
- The file is **not encrypted**. Store it on an encrypted disk (BitLocker, FileVault,
  LUKS) or in an encrypted container if the device could be lost or shared.
- The desktop app keeps only UI preferences and a recent-files list in
  `%APPDATA%\OpenBooks` (Windows) or `~/.config/openbooks` (Linux) or
  `~/Library/Application Support/OpenBooks` (macOS).

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
