#include "openbooks/util.hpp"

#include <cctype>
#include <charconv>
#include <system_error>

namespace ob {

std::string trim(std::string_view s) {
    const char* ws = " \t\r\n";
    const auto begin = s.find_first_not_of(ws);
    if (begin == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(ws);
    return std::string(s.substr(begin, end - begin + 1));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split(std::string_view s, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(separator, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            return out;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += separator;
        out += parts[i];
    }
    return out;
}

std::optional<long long> parseInt(std::string_view s) {
    const std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    long long value = 0;
    const char* first = t.data();
    const char* last = t.data() + t.size();
    if (*first == '+') ++first;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last) return std::nullopt;
    return value;
}

std::size_t displayWidth(std::string_view s) {
    std::size_t width = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++width;
    }
    return width;
}

}  // namespace ob
