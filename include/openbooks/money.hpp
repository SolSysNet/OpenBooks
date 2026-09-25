#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace ob {

// A currency amount held as an exact integer number of cents.
// Floating point is never used for money anywhere in OpenBooks.
class Money {
public:
    constexpr Money() = default;
    static constexpr Money fromCents(std::int64_t cents) {
        Money m;
        m.cents_ = cents;
        return m;
    }

    // Accepts "1234.5", "-12", "$1,234.56" and accounting-style "(45.00)".
    // Rejects amounts with more than two significant decimal places.
    static std::optional<Money> parse(std::string_view text);

    constexpr std::int64_t cents() const { return cents_; }
    constexpr bool isZero() const { return cents_ == 0; }

    std::string str() const;        // plain, e.g. "-1234.50" (files, CSV)
    std::string formatted() const;  // grouped, e.g. "-1,234.50" (reports)

    constexpr Money operator-() const { return fromCents(-cents_); }
    Money& operator+=(Money o) { cents_ += o.cents_; return *this; }
    Money& operator-=(Money o) { cents_ -= o.cents_; return *this; }
    friend Money operator+(Money a, Money b) { return a += b; }
    friend Money operator-(Money a, Money b) { return a -= b; }

    friend constexpr bool operator==(Money a, Money b) { return a.cents_ == b.cents_; }
    friend constexpr bool operator!=(Money a, Money b) { return a.cents_ != b.cents_; }
    friend constexpr bool operator<(Money a, Money b) { return a.cents_ < b.cents_; }
    friend constexpr bool operator>(Money a, Money b) { return a.cents_ > b.cents_; }
    friend constexpr bool operator<=(Money a, Money b) { return a.cents_ <= b.cents_; }
    friend constexpr bool operator>=(Money a, Money b) { return a.cents_ >= b.cents_; }

private:
    std::int64_t cents_ = 0;
};

// A non-currency decimal with four fixed places, used for quantities and percentages.
class Decimal {
public:
    static constexpr std::int64_t kScale = 10000;

    constexpr Decimal() = default;
    static constexpr Decimal fromRaw(std::int64_t raw) {
        Decimal d;
        d.raw_ = raw;
        return d;
    }
    static constexpr Decimal fromInt(std::int64_t v) { return fromRaw(v * kScale); }
    static std::optional<Decimal> parse(std::string_view text);

    constexpr std::int64_t raw() const { return raw_; }
    constexpr bool isZero() const { return raw_ == 0; }
    std::string str() const;  // trailing zeros trimmed: "1.5", "8.25", "3"

    friend constexpr bool operator==(Decimal a, Decimal b) { return a.raw_ == b.raw_; }
    friend constexpr bool operator!=(Decimal a, Decimal b) { return a.raw_ != b.raw_; }
    friend constexpr bool operator<(Decimal a, Decimal b) { return a.raw_ < b.raw_; }

private:
    std::int64_t raw_ = 0;
};

// amount * quantity, rounded half away from zero to the nearest cent.
Money multiply(Money amount, Decimal quantity);
// amount * percent / 100, rounded half away from zero to the nearest cent.
Money percentOf(Money amount, Decimal percent);

std::ostream& operator<<(std::ostream& out, Money m);
std::ostream& operator<<(std::ostream& out, Decimal d);

}  // namespace ob
