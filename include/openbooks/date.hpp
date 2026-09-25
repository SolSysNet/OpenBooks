#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace ob {

// A calendar date (proleptic Gregorian), stored as days since 1970-01-01.
class Date {
public:
    constexpr Date() = default;  // 1970-01-01

    static bool isValid(int year, unsigned month, unsigned day);
    static Date fromYMD(int year, unsigned month, unsigned day);  // throws std::invalid_argument
    // Accepts "YYYY-MM-DD", "MM/DD/YYYY" and "today".
    static std::optional<Date> parse(std::string_view text);
    static Date today();

    int year() const;
    unsigned month() const;
    unsigned day() const;
    constexpr int serial() const { return days_; }

    Date addDays(int n) const {
        Date d;
        d.days_ = days_ + n;
        return d;
    }
    std::string str() const;  // "YYYY-MM-DD"

    friend constexpr bool operator==(Date a, Date b) { return a.days_ == b.days_; }
    friend constexpr bool operator!=(Date a, Date b) { return a.days_ != b.days_; }
    friend constexpr bool operator<(Date a, Date b) { return a.days_ < b.days_; }
    friend constexpr bool operator>(Date a, Date b) { return a.days_ > b.days_; }
    friend constexpr bool operator<=(Date a, Date b) { return a.days_ <= b.days_; }
    friend constexpr bool operator>=(Date a, Date b) { return a.days_ >= b.days_; }
    friend constexpr int operator-(Date a, Date b) { return a.days_ - b.days_; }

private:
    int days_ = 0;
};

std::ostream& operator<<(std::ostream& out, Date d);

}  // namespace ob
