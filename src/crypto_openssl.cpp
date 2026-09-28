// Crypto primitives from the system's preinstalled OpenSSL libcrypto (Linux, macOS).
// OpenSSL is a hard build requirement on these platforms; it is never downloaded.

#include "openbooks/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>

#include <climits>
#include <cstring>
#include <memory>

namespace ob::crypto {
namespace {

void check(int result, const char* what) {
    if (result != 1) throw Error(std::string("encryption error (") + what + ")");
}

int intSize(std::size_t n) {
    if (n > static_cast<std::size_t>(INT_MAX)) throw Error("data too large to encrypt");
    return static_cast<int>(n);
}

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

CipherCtx newContext() {
    CipherCtx ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    if (!ctx) throw Error("encryption error (context)");
    return ctx;
}

const unsigned char* bytes(std::string_view s) { return reinterpret_cast<const unsigned char*>(s.data()); }

}  // namespace

const char* backendName() { return OPENSSL_VERSION_TEXT; }

void randomBytes(unsigned char* out, std::size_t size) { check(RAND_bytes(out, intSize(size)), "random"); }

void secureWipe(void* data, std::size_t size) {
    if (data && size) OPENSSL_cleanse(data, size);
}

void pbkdf2Sha256(std::string_view password, const unsigned char* salt, std::size_t saltSize, std::uint32_t iterations,
                  unsigned char* out, std::size_t outSize) {
    if (iterations > static_cast<std::uint32_t>(INT_MAX)) throw Error("invalid key derivation setting");
    check(PKCS5_PBKDF2_HMAC(password.data(), intSize(password.size()), salt, intSize(saltSize),
                            static_cast<int>(iterations), EVP_sha256(), intSize(outSize), out),
          "key derivation");
}

std::string aes256GcmEncrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                             std::string_view plaintext) {
    CipherCtx ctx = newContext();
    check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "init");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr), "nonce size");
    check(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key, nonce), "key");
    int len = 0;
    if (!aad.empty()) check(EVP_EncryptUpdate(ctx.get(), nullptr, &len, bytes(aad), intSize(aad.size())), "aad");
    std::string out(plaintext.size() + kTagSize, '\0');
    auto* data = reinterpret_cast<unsigned char*>(&out[0]);
    int written = 0;
    if (!plaintext.empty())
        check(EVP_EncryptUpdate(ctx.get(), data, &written, bytes(plaintext), intSize(plaintext.size())), "encrypt");
    check(EVP_EncryptFinal_ex(ctx.get(), data + written, &len), "final");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize), data + plaintext.size()),
          "tag");
    return out;
}

std::optional<std::string> aes256GcmDecrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                                            std::string_view ciphertextAndTag) {
    if (ciphertextAndTag.size() < kTagSize) return std::nullopt;
    const std::size_t size = ciphertextAndTag.size() - kTagSize;
    CipherCtx ctx = newContext();
    check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "init");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr), "nonce size");
    check(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key, nonce), "key");
    int len = 0;
    if (!aad.empty()) check(EVP_DecryptUpdate(ctx.get(), nullptr, &len, bytes(aad), intSize(aad.size())), "aad");
    std::string out(size, '\0');
    auto* data = reinterpret_cast<unsigned char*>(&out[0]);
    int written = 0;
    if (size) check(EVP_DecryptUpdate(ctx.get(), data, &written, bytes(ciphertextAndTag), intSize(size)), "decrypt");
    unsigned char tag[kTagSize];
    std::memcpy(tag, ciphertextAndTag.data() + size, kTagSize);
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), tag), "tag");
    if (EVP_DecryptFinal_ex(ctx.get(), data + written, &len) != 1) {  // authentication failed
        wipe(out);
        return std::nullopt;
    }
    return out;
}

}  // namespace ob::crypto
