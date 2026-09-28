// Crypto primitives from Windows CNG (bcrypt.dll), which ships with every supported Windows.

#include "openbooks/crypto.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <climits>

namespace ob::crypto {
namespace {

constexpr NTSTATUS kAuthTagMismatch = static_cast<NTSTATUS>(0xC000A002L);  // STATUS_AUTH_TAG_MISMATCH

bool ok(NTSTATUS s) { return s >= 0; }

void check(NTSTATUS s, const char* what) {
    if (!ok(s)) throw Error(std::string("encryption error (") + what + ")");
}

ULONG ulong(std::size_t n) {
    if (n > ULONG_MAX) throw Error("data too large to encrypt");
    return static_cast<ULONG>(n);
}

// RAII wrappers so every handle is released, even when a call throws.
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    Algorithm(LPCWSTR id, ULONG flags) { check(BCryptOpenAlgorithmProvider(&handle, id, nullptr, flags), "provider"); }
    ~Algorithm() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
};

struct GcmKey {
    Algorithm aes{BCRYPT_AES_ALGORITHM, 0};
    BCRYPT_KEY_HANDLE handle = nullptr;
    explicit GcmKey(const unsigned char* key) {
        check(BCryptSetProperty(aes.handle, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
              "GCM mode");
        check(BCryptGenerateSymmetricKey(aes.handle, &handle, nullptr, 0, const_cast<PUCHAR>(key), ulong(kKeySize), 0),
              "key");
    }
    ~GcmKey() {
        if (handle) BCryptDestroyKey(handle);
    }
    GcmKey(const GcmKey&) = delete;
    GcmKey& operator=(const GcmKey&) = delete;
};

PUCHAR bytes(std::string_view s) { return reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())); }

}  // namespace

const char* backendName() { return "Windows CNG"; }

void randomBytes(unsigned char* out, std::size_t size) {
    check(BCryptGenRandom(nullptr, out, ulong(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "random");
}

void secureWipe(void* data, std::size_t size) {
    if (data && size) SecureZeroMemory(data, size);
}

void pbkdf2Sha256(std::string_view password, const unsigned char* salt, std::size_t saltSize, std::uint32_t iterations,
                  unsigned char* out, std::size_t outSize) {
    Algorithm hmac(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    check(BCryptDeriveKeyPBKDF2(hmac.handle, bytes(password), ulong(password.size()), const_cast<PUCHAR>(salt),
                                ulong(saltSize), iterations, out, ulong(outSize), 0),
          "key derivation");
}

std::string aes256GcmEncrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                             std::string_view plaintext) {
    GcmKey k(key);
    std::string out(plaintext.size() + kTagSize, '\0');
    auto* data = reinterpret_cast<PUCHAR>(&out[0]);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = ulong(kNonceSize);
    info.pbAuthData = bytes(aad);
    info.cbAuthData = ulong(aad.size());
    info.pbTag = data + plaintext.size();
    info.cbTag = ulong(kTagSize);
    ULONG written = 0;
    check(BCryptEncrypt(k.handle, bytes(plaintext), ulong(plaintext.size()), &info, nullptr, 0, data,
                        ulong(plaintext.size()), &written, 0),
          "encrypt");
    return out;
}

std::optional<std::string> aes256GcmDecrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                                            std::string_view ciphertextAndTag) {
    if (ciphertextAndTag.size() < kTagSize) return std::nullopt;
    const std::size_t size = ciphertextAndTag.size() - kTagSize;
    GcmKey k(key);
    std::string out(size, '\0');
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = ulong(kNonceSize);
    info.pbAuthData = bytes(aad);
    info.cbAuthData = ulong(aad.size());
    info.pbTag = bytes(ciphertextAndTag) + size;
    info.cbTag = ulong(kTagSize);
    ULONG written = 0;
    const NTSTATUS s = BCryptDecrypt(k.handle, bytes(ciphertextAndTag), ulong(size), &info, nullptr, 0,
                                     size ? reinterpret_cast<PUCHAR>(&out[0]) : nullptr, ulong(size), &written, 0);
    if (s == kAuthTagMismatch) {
        wipe(out);
        return std::nullopt;
    }
    check(s, "decrypt");
    return out;
}

}  // namespace ob::crypto
