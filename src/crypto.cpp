// Platform-independent part of books-file encryption: the file format and key handling.
// The actual primitives live in crypto_cng.cpp (Windows) or crypto_openssl.cpp (elsewhere).

#include "openbooks/crypto.hpp"

#include <cstring>

namespace ob {
namespace {

constexpr char kMagic[8] = {'O', 'B', 'K', 'C', 'R', 'Y', 'P', 'T'};
constexpr unsigned char kFormatVersion = 1;
constexpr unsigned char kKdfPbkdf2Sha256 = 1;
constexpr unsigned char kCipherAes256Gcm = 1;

struct Header {
    std::uint32_t iterations = 0;
    std::array<unsigned char, crypto::kSaltSize> salt{};
    std::array<unsigned char, crypto::kNonceSize> nonce{};
};

std::string buildHeader(const Header& h) {
    std::string out(kMagic, sizeof kMagic);
    out += static_cast<char>(kFormatVersion);
    out += static_cast<char>(kKdfPbkdf2Sha256);
    for (int i = 0; i < 4; ++i) out += static_cast<char>((h.iterations >> (8 * i)) & 0xFF);
    out.append(reinterpret_cast<const char*>(h.salt.data()), h.salt.size());
    out += static_cast<char>(kCipherAes256Gcm);
    out.append(reinterpret_cast<const char*>(h.nonce.data()), h.nonce.size());
    return out;
}

Header parseHeader(std::string_view file) {
    if (file.size() < crypto::kHeaderSize + crypto::kTagSize || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0)
        throw Error("this is not an encrypted OpenBooks file, or it is truncated");
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(file[i]); };
    if (byte(8) != kFormatVersion) throw Error("this encrypted file needs a newer version of OpenBooks");
    if (byte(9) != kKdfPbkdf2Sha256 || byte(30) != kCipherAes256Gcm)
        throw Error("this encrypted file uses an unsupported algorithm");
    Header h;
    for (int i = 0; i < 4; ++i) h.iterations |= static_cast<std::uint32_t>(byte(10 + static_cast<std::size_t>(i))) << (8 * i);
    if (h.iterations < crypto::kMinIterations || h.iterations > crypto::kMaxIterations)
        throw Error("this encrypted file has invalid key settings");
    std::memcpy(h.salt.data(), file.data() + 14, h.salt.size());
    std::memcpy(h.nonce.data(), file.data() + 31, h.nonce.size());
    return h;
}

}  // namespace

void crypto::wipe(std::string& s) {
    if (!s.empty()) secureWipe(&s[0], s.size());
    s.clear();
}

std::unique_ptr<FileKey> FileKey::fromNewPassword(std::string_view password, std::uint32_t iterations) {
    if (password.size() < crypto::kMinPasswordLength)
        throw Error("the password must be at least " + std::to_string(crypto::kMinPasswordLength) + " characters");
    if (iterations < crypto::kMinIterations || iterations > crypto::kMaxIterations)
        throw Error("invalid key derivation setting");
    std::unique_ptr<FileKey> key(new FileKey());
    key->iterations_ = iterations;
    crypto::randomBytes(key->salt_.data(), key->salt_.size());
    crypto::pbkdf2Sha256(password, key->salt_.data(), key->salt_.size(), iterations, key->key_.data(), key->key_.size());
    return key;
}

bool FileKey::isEncrypted(std::string_view fileBytes) {
    return fileBytes.size() >= sizeof kMagic && std::memcmp(fileBytes.data(), kMagic, sizeof kMagic) == 0;
}

std::string FileKey::decryptFile(std::string_view fileBytes, std::string_view password, std::unique_ptr<FileKey>* keyOut) {
    const Header h = parseHeader(fileBytes);
    std::unique_ptr<FileKey> key(new FileKey());
    key->iterations_ = h.iterations;
    key->salt_ = h.salt;
    crypto::pbkdf2Sha256(password, h.salt.data(), h.salt.size(), h.iterations, key->key_.data(), key->key_.size());
    const std::string_view header = fileBytes.substr(0, crypto::kHeaderSize);
    auto plaintext = crypto::aes256GcmDecrypt(key->key_.data(), h.nonce.data(), header, fileBytes.substr(crypto::kHeaderSize));
    if (!plaintext) throw WrongPasswordError("wrong password, or the file has been damaged or altered");
    if (keyOut) *keyOut = std::move(key);
    return std::move(*plaintext);
}

std::string FileKey::encryptFile(std::string_view plaintext) const {
    Header h;
    h.iterations = iterations_;
    h.salt = salt_;
    crypto::randomBytes(h.nonce.data(), h.nonce.size());  // never reused: fresh for every save
    const std::string header = buildHeader(h);
    return header + crypto::aes256GcmEncrypt(key_.data(), h.nonce.data(), header, plaintext);
}

std::string FileKey::decryptFile(std::string_view fileBytes) const {
    const Header h = parseHeader(fileBytes);
    if (h.iterations != iterations_ || h.salt != salt_)
        throw WrongPasswordError("the file's password was changed elsewhere; open it again");
    auto plaintext = crypto::aes256GcmDecrypt(key_.data(), h.nonce.data(), fileBytes.substr(0, crypto::kHeaderSize),
                                              fileBytes.substr(crypto::kHeaderSize));
    if (!plaintext) throw WrongPasswordError("the file has been damaged or altered");
    return std::move(*plaintext);
}

bool FileKey::matches(std::string_view password) const {
    std::array<unsigned char, crypto::kKeySize> candidate{};
    crypto::pbkdf2Sha256(password, salt_.data(), salt_.size(), iterations_, candidate.data(), candidate.size());
    unsigned char diff = 0;
    for (std::size_t i = 0; i < candidate.size(); ++i) diff |= static_cast<unsigned char>(candidate[i] ^ key_[i]);
    crypto::secureWipe(candidate.data(), candidate.size());
    return diff == 0;
}

FileKey::~FileKey() { crypto::secureWipe(key_.data(), key_.size()); }

}  // namespace ob
