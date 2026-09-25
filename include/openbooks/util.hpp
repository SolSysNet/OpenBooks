#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

std::string trim(std::string_view s);
std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool startsWith(std::string_view s, std::string_view prefix);
std::vector<std::string> split(std::string_view s, char separator);
std::string join(const std::vector<std::string>& parts, std::string_view separator);
std::optional<long long> parseInt(std::string_view s);

// Number of terminal columns a UTF-8 string occupies (one per code point).
std::size_t displayWidth(std::string_view s);

}  // namespace ob
