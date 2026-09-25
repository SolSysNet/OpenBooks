#include "openbooks/money.hpp"

#include <ostream>

namespace ob {
namespace {

constexpr std::int64_t kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
constexpr std::int64_t kMaxWhole = 100000000000000LL;  // 1e14 keeps scaled values well inside int64

std::optional<std::int64_t> parseFixed(std::string_view s, int decimals) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);

    bool negative = false;
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        negative = true;
        s = s.substr(1, s.size() - 2);
    }
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        if (s.front() == '-') negative = !negative;
        s.remove_prefix(1);
    }
    if (!s.empty() && s.front() == '$') s.remove_prefix(1);

    std::int64_t whole = 0;
    std::int64_t frac = 0;
    int fracDigits = 0;
    bool seenDot = false;
    bool anyDigit = false;
    for (char c : s) {
        if (c == ',' && !seenDot) continue;
        if (c == '.') {
            if (seenDot) return std::nullopt;
            seenDot = true;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        anyDigit = true;
        const int digit = c - '0';
        if (seenDot) {
            if (fracDigits == decimals) {
                if (digit != 0) return std::nullopt;  // would silently lose precision
                continue;
            }
            frac = frac * 10 + digit;
            ++fracDigits;
        } else {
            if (whole > kMaxWhole) return std::nullopt;
            whole = whole * 10 + digit;
        }
    }
    if (!anyDigit) return std::nullopt;
    while (fracDigits < decimals) {
        frac *= 10;
        ++fracDigits;
    }
    const std::int64_t value = whole * kPow10[decimals] + frac;
    return negative ? -value : value;
}

std::string formatFixed(std::int64_t v, int decimals, bool grouping, bool trimZeros) {
    const bool negative = v < 0;
    const std::uint64_t u = negative ? 0ULL - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    const auto scale = static_cast<std::uint64_t>(kPow10[decimals]);

    std::string whole = std::to_string(u / scale);
    std::string frac = std::to_string(u % scale);
    frac.insert(0, static_cast<std::size_t>(decimals) - frac.size(), '0');
    if (trimZeros) {
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
    }
    if (grouping && whole.size() > 3) {
        std::string grouped;
        int count = 0;
        for (auto it = whole.rbegin(); it != whole.rend(); ++it) {
            if (count > 0 && count % 3 == 0) grouped.push_back(',');
            grouped.push_back(*it);
            ++count;
        }
        whole.assign(grouped.rbegin(), grouped.rend());
    }
    std::string out = negative ? "-" : "";
    out += whole;
    if (!frac.empty()) {
        out += '.';
        out += frac;
    }
    return out;
}

// Integer division rounding half away from zero; den must be positive.
std::int64_t divRound(std::int64_t num, std::int64_t den) {
    std::int64_t q = num / den;
    const std::int64_t r = num % den;
    if (2 * (r < 0 ? -r : r) >= den) q += (num < 0 ? -1 : 1);
    return q;
}

}  // namespace

std::optional<Money> Money::parse(std::string_view text) {
    auto v = parseFixed(text, 2);
    if (!v) return std::nullopt;
    return fromCents(*v);
}

std::string Money::str() const { return formatFixed(cents_, 2, false, false); }
std::string Money::formatted() const { return formatFixed(cents_, 2, true, false); }

std::optional<Decimal> Decimal::parse(std::string_view text) {
    auto v = parseFixed(text, 4);
    if (!v) return std::nullopt;
    return fromRaw(*v);
}

std::string Decimal::str() const { return formatFixed(raw_, 4, false, true); }

Money multiply(Money amount, Decimal quantity) {
    return Money::fromCents(divRound(amount.cents() * quantity.raw(), Decimal::kScale));
}

Money percentOf(Money amount, Decimal percent) {
    return Money::fromCents(divRound(amount.cents() * percent.raw(), Decimal::kScale * 100));
}

std::ostream& operator<<(std::ostream& out, Money m) { return out << m.str(); }
std::ostream& operator<<(std::ostream& out, Decimal d) { return out << d.str(); }

}  // namespace ob
