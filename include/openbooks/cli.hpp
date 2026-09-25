#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace ob {

constexpr const char* kVersion = "0.2.0";

// Runs one OpenBooks command line (argv without the program name). Returns the exit code.
int runCli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, std::istream& in);

// Splits a shell-style command line into arguments ("double" and 'single' quotes supported).
std::vector<std::string> tokenize(const std::string& line);

}  // namespace ob
