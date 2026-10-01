#pragma once

// JNI plumbing shared by the bridge and the Android crypto backend.

#include <jni.h>

namespace jx {

// Called from JNI_OnLoad: remembers the VM and caches org.openbooks.core.NativeCrypto.
bool initialize(JavaVM* vm, JNIEnv* env);

// The calling thread's JNIEnv (all bridge calls arrive on Java threads).
JNIEnv* env();

jclass cryptoClass();
jmethodID cryptoRandom();
jmethodID cryptoPbkdf2();
jmethodID cryptoEncrypt();
jmethodID cryptoDecrypt();

// Deletes a JNI local reference when it goes out of scope.
template <typename T>
class LocalRef {
public:
    LocalRef(JNIEnv* env, T ref) : env_(env), ref_(ref) {}
    ~LocalRef() {
        if (ref_) env_->DeleteLocalRef(ref_);
    }
    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;
    T get() const { return ref_; }

private:
    JNIEnv* env_;
    T ref_;
};

}  // namespace jx
