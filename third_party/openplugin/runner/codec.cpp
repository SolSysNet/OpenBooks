#include "codec.hpp"

#include "openplugin/sha256.hpp"

#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace opl::runner {

// ---------------------------------------------------------------------- money

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

[[noreturn]] void overflow() { throw Error("amount out of range"); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::optional<std::int64_t> parseMoney(std::string_view s) {
    bool negative = false;
    if (!s.empty() && s[0] == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    const std::size_t dot = s.find('.');
    const std::string_view whole = s.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 16) return std::nullopt;
    if (dot != std::string_view::npos && (frac.empty() || frac.size() > 2)) return std::nullopt;
    std::int64_t cents = 0;
    for (char c : whole) {
        if (!isDigit(c)) return std::nullopt;
        cents = cents * 10 + (c - '0');
    }
    cents *= 100;
    if (frac.size() >= 1) {
        if (!isDigit(frac[0])) return std::nullopt;
        cents += (frac[0] - '0') * 10;
    }
    if (frac.size() == 2) {
        if (!isDigit(frac[1])) return std::nullopt;
        cents += frac[1] - '0';
    }
    return negative ? -cents : cents;
}

std::string formatMoney(std::int64_t cents) {
    const bool negative = cents < 0;
    // Work in unsigned so INT64_MIN formats correctly.
    const std::uint64_t mag = negative ? std::uint64_t(0) - static_cast<std::uint64_t>(cents) : static_cast<std::uint64_t>(cents);
    std::string frac = std::to_string(mag % 100);
    if (frac.size() < 2) frac.insert(0, "0");
    return (negative ? "-" : "") + std::to_string(mag / 100) + "." + frac;
}

std::int64_t addMoney(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) overflow();
    return a + b;
}

std::int64_t subMoney(std::int64_t a, std::int64_t b) {
    if ((b < 0 && a > kMax + b) || (b > 0 && a < kMin + b)) overflow();
    return a - b;
}

std::int64_t mulMoney(std::int64_t cents, std::string_view factor) {
    // factor = raw / 10^scale, with raw an integer.
    bool negative = false;
    if (!factor.empty() && factor[0] == '-') {
        negative = true;
        factor.remove_prefix(1);
    }
    const std::size_t dot = factor.find('.');
    const std::string_view whole = factor.substr(0, dot);
    const std::string_view frac = dot == std::string_view::npos ? std::string_view() : factor.substr(dot + 1);
    if (whole.empty() || whole.size() > 12 || frac.size() > 6 || (dot != std::string_view::npos && frac.empty()))
        throw Error("invalid factor '" + std::string(factor) + "'");
    std::int64_t raw = 0;
    for (char c : whole) {
        if (!isDigit(c)) throw Error("invalid factor '" + std::string(factor) + "'");
        raw = raw * 10 + (c - '0');
    }
    std::int64_t scale = 1;
    for (char c : frac) {
        if (!isDigit(c)) throw Error("invalid factor '" + std::string(factor) + "'");
        raw = raw * 10 + (c - '0');
        scale *= 10;
    }
    if (raw == 0 || cents == 0) return 0;

    // |cents| * raw as a 128-bit number in four 32-bit limbs (most significant first), divided by
    // scale (at most 10^6) with long division. One portable path for every compiler.
    const bool resultNegative = (cents < 0) != negative;
    const std::uint64_t a = cents < 0 ? std::uint64_t(0) - static_cast<std::uint64_t>(cents) : static_cast<std::uint64_t>(cents);
    const std::uint64_t b = static_cast<std::uint64_t>(raw);
    const std::uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32, bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
    const std::uint64_t ll = aLo * bLo, lh = aLo * bHi, hl = aHi * bLo, hh = aHi * bHi;
    const std::uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
    const std::uint64_t low = (mid << 32) | (ll & 0xFFFFFFFFu);
    const std::uint64_t high = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    const std::uint32_t limbs[4] = {static_cast<std::uint32_t>(high >> 32), static_cast<std::uint32_t>(high),
                                    static_cast<std::uint32_t>(low >> 32), static_cast<std::uint32_t>(low)};
    const std::uint64_t d = static_cast<std::uint64_t>(scale);
    std::uint64_t rem = 0;
    std::uint32_t q[4];
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t cur = (rem << 32) | limbs[i];
        q[i] = static_cast<std::uint32_t>(cur / d);
        rem = cur % d;
    }
    if (q[0] != 0 || q[1] != 0) overflow();
    std::uint64_t quotient = (std::uint64_t(q[2]) << 32) | q[3];
    if (2 * rem >= d) ++quotient;  // half away from zero (we're working with magnitudes)
    if (quotient > static_cast<std::uint64_t>(kMax)) overflow();
    return resultNegative ? -static_cast<std::int64_t>(quotient) : static_cast<std::int64_t>(quotient);
}

// ------------------------------------------------------------------ encodings

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t(std::uint8_t(data[i])) << 16) | (std::uint32_t(std::uint8_t(data[i + 1])) << 8) |
                                std::uint8_t(data[i + 2]);
        out += kB64[n >> 18];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += kB64[n & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t n = std::uint32_t(std::uint8_t(data[i])) << 16;
        out += kB64[n >> 18];
        out += kB64[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t n = (std::uint32_t(std::uint8_t(data[i])) << 16) | (std::uint32_t(std::uint8_t(data[i + 1])) << 8);
        out += kB64[n >> 18];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=' && last && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                if (pad) return std::nullopt;  // data after padding
                v[k] = value(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const std::uint32_t n = (std::uint32_t(v[0]) << 18) | (std::uint32_t(v[1]) << 12) | (std::uint32_t(v[2]) << 6) | std::uint32_t(v[3]);
        out += static_cast<char>(n >> 16);
        if (pad < 2) out += static_cast<char>((n >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(n & 0xFF);
    }
    return out;
}

std::string hexEncode(std::string_view data) {
    return toHex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::optional<std::string> hexDecode(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out += static_cast<char>((hi << 4) | lo);
    }
    return out;
}

// -------------------------------------------------------------------- hashing

std::string sha256Raw(std::string_view data) {
    Sha256 h;
    h.update(data);
    const auto d = h.finish();
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

std::string hmacSha256Raw(std::string_view key, std::string_view message) {
    std::string k = key.size() > 64 ? sha256Raw(key) : std::string(key);
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<char>(k[i] ^ 0x36);
        opad[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(message);
    const auto innerDigest = inner.finish();
    Sha256 outer;
    outer.update(opad);
    outer.update(innerDigest.data(), innerDigest.size());
    const auto d = outer.finish();
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

std::string randomBytes(std::size_t n) {
    std::string out(n, '\0');
    if (n == 0) return out;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw Error("the system random number generator failed");
#else
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Error("the system random number generator is unavailable");
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, out.data() + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            ::close(fd);
            throw Error("the system random number generator failed");
        }
        got += static_cast<std::size_t>(r);
    }
    ::close(fd);
#endif
    return out;
}

}  // namespace opl::runner
