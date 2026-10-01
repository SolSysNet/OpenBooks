// Crypto primitives for Android: calls org.openbooks.core.NativeCrypto, which uses the
// platform's javax.crypto provider. Same algorithms and file format as the desktop backends
// (crypto_cng.cpp, crypto_openssl.cpp).

#include "openbooks/crypto.hpp"

#include "jni_env.hpp"

#include <cstring>

namespace ob::crypto {
namespace {

using jx::LocalRef;

jbyteArray toJava(JNIEnv* env, const unsigned char* data, std::size_t size) {
    jbyteArray a = env->NewByteArray(static_cast<jsize>(size));
    if (!a) throw Error("out of memory");
    if (size) env->SetByteArrayRegion(a, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
    return a;
}

jbyteArray toJava(JNIEnv* env, std::string_view s) {
    return toJava(env, reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

std::string fromJava(JNIEnv* env, jbyteArray a) {
    const jsize n = env->GetArrayLength(a);
    std::string out(static_cast<std::size_t>(n), '\0');
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(&out[0]));
    return out;
}

// Converts a pending Java exception into an ob::Error.
void rethrow(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return;
    env->ExceptionClear();
    throw Error(std::string("encryption error (") + what + ")");
}

}  // namespace

const char* backendName() { return "Android javax.crypto"; }

void randomBytes(unsigned char* out, std::size_t size) {
    JNIEnv* env = jx::env();
    LocalRef<jbyteArray> result(env, static_cast<jbyteArray>(
                                         env->CallStaticObjectMethod(jx::cryptoClass(), jx::cryptoRandom(), static_cast<jint>(size))));
    rethrow(env, "random");
    const std::string bytes = fromJava(env, result.get());
    if (bytes.size() != size) throw Error("encryption error (random)");
    std::memcpy(out, bytes.data(), size);
}

void secureWipe(void* data, std::size_t size) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}

void pbkdf2Sha256(std::string_view password, const unsigned char* salt, std::size_t saltSize, std::uint32_t iterations,
                  unsigned char* out, std::size_t outSize) {
    JNIEnv* env = jx::env();
    LocalRef<jbyteArray> pw(env, toJava(env, password));
    LocalRef<jbyteArray> s(env, toJava(env, salt, saltSize));
    LocalRef<jbyteArray> result(env, static_cast<jbyteArray>(env->CallStaticObjectMethod(
                                         jx::cryptoClass(), jx::cryptoPbkdf2(), pw.get(), s.get(),
                                         static_cast<jint>(iterations), static_cast<jint>(outSize))));
    rethrow(env, "key derivation");
    std::string key = fromJava(env, result.get());
    if (key.size() != outSize) throw Error("encryption error (key derivation)");
    std::memcpy(out, key.data(), outSize);
    wipe(key);
}

std::string aes256GcmEncrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                             std::string_view plaintext) {
    JNIEnv* env = jx::env();
    LocalRef<jbyteArray> k(env, toJava(env, key, kKeySize));
    LocalRef<jbyteArray> n(env, toJava(env, nonce, kNonceSize));
    LocalRef<jbyteArray> a(env, toJava(env, aad));
    LocalRef<jbyteArray> p(env, toJava(env, plaintext));
    LocalRef<jbyteArray> result(env, static_cast<jbyteArray>(env->CallStaticObjectMethod(
                                         jx::cryptoClass(), jx::cryptoEncrypt(), k.get(), n.get(), a.get(), p.get())));
    rethrow(env, "encrypt");
    return fromJava(env, result.get());
}

std::optional<std::string> aes256GcmDecrypt(const unsigned char* key, const unsigned char* nonce, std::string_view aad,
                                            std::string_view ciphertextAndTag) {
    if (ciphertextAndTag.size() < kTagSize) return std::nullopt;
    JNIEnv* env = jx::env();
    LocalRef<jbyteArray> k(env, toJava(env, key, kKeySize));
    LocalRef<jbyteArray> n(env, toJava(env, nonce, kNonceSize));
    LocalRef<jbyteArray> a(env, toJava(env, aad));
    LocalRef<jbyteArray> c(env, toJava(env, ciphertextAndTag));
    LocalRef<jbyteArray> result(env, static_cast<jbyteArray>(env->CallStaticObjectMethod(
                                         jx::cryptoClass(), jx::cryptoDecrypt(), k.get(), n.get(), a.get(), c.get())));
    rethrow(env, "decrypt");
    if (!result.get()) return std::nullopt;  // authentication failed
    return fromJava(env, result.get());
}

}  // namespace ob::crypto
