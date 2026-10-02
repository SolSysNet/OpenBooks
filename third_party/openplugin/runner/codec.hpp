#pragma once

// Helpers behind op.money, op.base64, op.hex, op.sha256, op.hmac_sha256 and op.random_bytes. Plain
// C++ with no Lua, so they are tested directly.

#include "openplugin/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace opl::runner {

// ---- Money: exact integer cents. Text is "-1234.50" style: an optional '-', digits, and optionally
// '.' with one or two digits. Output always has exactly two decimals. Rounding is half away from
// zero, the same as the apps.

std::optional<std::int64_t> parseMoney(std::string_view s);
std::string formatMoney(std::int64_t cents);
// Throws opl::Error on overflow.
std::int64_t addMoney(std::int64_t a, std::int64_t b);
std::int64_t subMoney(std::int64_t a, std::int64_t b);
// cents * factor, where factor is a decimal string ("1.5", "-0.0725", "3"), up to 6 decimal places.
std::int64_t mulMoney(std::int64_t cents, std::string_view factor);

// ---- Encodings
std::string base64Encode(std::string_view data);
std::optional<std::string> base64Decode(std::string_view text);  // standard alphabet, padding required
std::string hexEncode(std::string_view data);
std::optional<std::string> hexDecode(std::string_view text);     // either case

// ---- Hashing (raw bytes out)
std::string sha256Raw(std::string_view data);
std::string hmacSha256Raw(std::string_view key, std::string_view message);

// From the OS's cryptographic random source. Throws opl::Error if it fails.
std::string randomBytes(std::size_t n);

}  // namespace opl::runner
