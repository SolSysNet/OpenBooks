#include "openbooks/date.hpp"

#include "openbooks/util.hpp"

#include <cstdio>
#include <ctime>
#include <ostream>
#include <stdexcept>

namespace ob {
namespace {

// Algorithms from Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms".
int daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

struct Civil {
    int y;
    unsigned m;
    unsigned d;
};

Civil civilFromDays(int z) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {y + (m <= 2 ? 1 : 0), m, d};
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned daysInMonth(int y, unsigned m) {
    static const unsigned table[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && isLeap(y)) ? 29 : table[m - 1];
}

}  // namespace

bool Date::isValid(int year, unsigned month, unsigned day) {
    return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 &&
           day <= daysInMonth(year, month);
}

Date Date::fromYMD(int year, unsigned month, unsigned day) {
    if (!isValid(year, month, day)) throw std::invalid_argument("invalid date");
    Date d;
    d.days_ = daysFromCivil(year, month, day);
    return d;
}

std::optional<Date> Date::parse(std::string_view text) {
    const std::string t = trim(text);
    if (iequals(t, "today")) return today();

    std::vector<std::string> parts;
    bool usFormat = false;
    if (t.find('-') != std::string::npos) {
        parts = split(t, '-');
    } else if (t.find('/') != std::string::npos) {
        parts = split(t, '/');
        usFormat = true;
    }
    if (parts.size() != 3) return std::nullopt;

    const auto a = parseInt(parts[0]);
    const auto b = parseInt(parts[1]);
    const auto c = parseInt(parts[2]);
    if (!a || !b || !c) return std::nullopt;
    const long long y = usFormat ? *c : *a;
    const long long m = usFormat ? *a : *b;
    const long long d = usFormat ? *b : *c;
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    if (!isValid(static_cast<int>(y), static_cast<unsigned>(m), static_cast<unsigned>(d))) return std::nullopt;
    return fromYMD(static_cast<int>(y), static_cast<unsigned>(m), static_cast<unsigned>(d));
}

Date Date::today() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return fromYMD(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                   static_cast<unsigned>(local.tm_mday));
}

int Date::year() const { return civilFromDays(days_).y; }
unsigned Date::month() const { return civilFromDays(days_).m; }
unsigned Date::day() const { return civilFromDays(days_).d; }

std::string Date::str() const {
    const Civil c = civilFromDays(days_);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", c.y, c.m, c.d);
    return buf;
}

std::ostream& operator<<(std::ostream& out, Date d) { return out << d.str(); }

}  // namespace ob
