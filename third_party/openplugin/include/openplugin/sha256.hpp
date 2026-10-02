#pragma once

// SHA-256 (FIPS 180-4), used to pin the exact plugin files a user approved. Plain portable code:
// hash pinning must give the same answer on every platform, and this isn't a secret-handling
// primitive, so it doesn't need the platform crypto libraries the apps use for encryption.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace opl {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    std::array<std::uint8_t, 32> finish();  // the object can't be reused afterwards

private:
    void block(const std::uint8_t* p);

    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

std::string toHex(const std::uint8_t* data, std::size_t size);
std::string sha256Hex(std::string_view data);

// Hashes a file's contents. Throws opl::Error if it can't be read.
std::string sha256File(const std::filesystem::path& path);

}  // namespace opl
