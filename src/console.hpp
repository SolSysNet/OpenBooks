#pragma once

#include <string>

namespace ob {

// Reads one line from the interactive console without echoing it (for passwords), as UTF-8.
// Returns false when standard input is not an interactive console (a pipe, a file, tests).
bool readHiddenConsoleLine(std::string& line);

}  // namespace ob
