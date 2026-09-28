#pragma once

// Password-based encryption for books files.
//
// No cryptography is implemented here: the primitives come from the operating system's
// crypto provider (Windows CNG) or from the system's preinstalled OpenSSL libcrypto
// (Linux, macOS). Both backends use the same algorithms, so encrypted files move freely
// between platforms:
//
//   key   = PBKDF2-HMAC-SHA256(password, 16-byte random salt, iterations)   (32 bytes)
//   file  = header || AES-256-GCM(key, 12-byte random nonce, aad = header, books text) || tag
//
// Encrypted file layout (integers little-endian):
//   0   8  magic "OBKCRYPT"
//   8   1  format version (1)
//   9   1  key derivation (1 = PBKDF2-HMAC-SHA256)
//   10  4  iterations
//   14  16 salt
//   30  1  cipher (1 = AES-256-GCM)
//   31  12 nonce
//   43  n  ciphertext
//   ..  16 authentication tag
// The 43-byte header is authenticated as additional data, so any change to it (for example
// lowering the iteration count) makes decryption fail.

#include "openbooks/book.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ob {

// The file is encrypted and no password was supplied.
class PasswordRequiredError : public Error {
public:
    using Error::Error;
};

// Decryption failed: wrong password, or the file was damaged or altered.
class WrongPasswordError : public Error {
public:
    using Error::Error;
};

namespace crypto {

constexpr std::size_t kKeySize = 32;
constexpr std::size_t kSaltSize = 16;
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kHeaderSize = 43;
constexpr std::uint32_t kDefaultIterations = 600000;  // OWASP 2023 guidance for PBKDF2-HMAC-SHA256
constexpr std::uint32_t kMinIterations = 100000;
constexpr std::uint32_t kMaxIterations = 10000000;  // refuse files that would stall us
constexpr std::size_t kMinPasswordLength = 8;

// ---- Primitives, provided by the platform backend (crypto_cng.cpp / crypto_openssl.cpp)
const char* backendName();
void randomBytes(unsigned char* out, std::size_t size);  // CSPRNG; throws on failure
void secureWipe(void* data, std::size_t size);           // not optimized away
void pbkdf2Sha256(std::string_view password, const unsigned char* salt, std::size_t saltSize, std::uint32_t iterations,
                  unsigned char* out, std::size_t outSize);
// Returns ciphertext || 16-byte tag.
std::string aes256GcmEncrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                             std::string_view plaintext);
// nullopt when authentication fails.
std::optional<std::string> aes256GcmDecrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                                            std::string_view ciphertextAndTag);

// Wipes a string's contents (e.g. a password) and empties it.
void wipe(std::string& s);

}  // namespace crypto

// The key protecting one books file. It is derived from the password once and kept only in
// memory; it can't be copied and is wiped when destroyed.
class FileKey {
public:
    // A key for a new password, with a fresh random salt. Throws if the password is too short.
    static std::unique_ptr<FileKey> fromNewPassword(std::string_view password,
                                                    std::uint32_t iterations = crypto::kDefaultIterations);
    static bool isEncrypted(std::string_view fileBytes);
    // Decrypts a whole encrypted file. On success optionally returns the key, so later saves
    // can reuse it without re-running the key derivation.
    static std::string decryptFile(std::string_view fileBytes, std::string_view password,
                                   std::unique_ptr<FileKey>* keyOut = nullptr);

    // Encrypts with this key and a fresh random nonce.
    std::string encryptFile(std::string_view plaintext) const;
    // Decrypts a file written with this key (no key derivation needed). Throws
    // WrongPasswordError if the file now uses a different password.
    std::string decryptFile(std::string_view fileBytes) const;
    // True when `password` derives this same key (constant-time comparison).
    bool matches(std::string_view password) const;
    std::uint32_t iterations() const { return iterations_; }

    FileKey(const FileKey&) = delete;
    FileKey& operator=(const FileKey&) = delete;
    ~FileKey();

private:
    FileKey() = default;
    std::array<unsigned char, crypto::kKeySize> key_{};
    std::array<unsigned char, crypto::kSaltSize> salt_{};
    std::uint32_t iterations_ = crypto::kDefaultIterations;
};

}  // namespace ob
