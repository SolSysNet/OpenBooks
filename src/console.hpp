#pragma once

#include <string>

namespace ob {

// Reads one line from the interactive console without echoing it (for passwords), as UTF-8.
// Returns false when standard input is not an interactive console (a pipe, a file, tests).
bool readHiddenConsoleLine(std::string& line);

// Reads an environment variable as UTF-8 (on Windows getenv() returns the ANSI code page, which
// would turn a non-ASCII password into different bytes than the one typed at the prompt).
// Returns an empty string when the variable is unset.
std::string environmentUtf8(const char* name);

}  // namespace ob
