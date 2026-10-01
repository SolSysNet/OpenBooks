package org.openbooks.core;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Crypto primitives for the native engine, backed by the platform's javax.crypto provider
 * (Conscrypt/BoringSSL on Android). Called from crypto_android.cpp over JNI; OpenBooks
 * implements no cryptography itself. Same algorithms as the desktop backends, so
 * encrypted books open on every platform.
 */
public final class NativeCrypto {
    private static final SecureRandom RANDOM = new SecureRandom();

    private NativeCrypto() {}

    public static byte[] random(int size) {
        byte[] out = new byte[size];
        RANDOM.nextBytes(out);
        return out;
    }

    /** PBKDF2-HMAC-SHA256. The password arrives as UTF-8 bytes, exactly as on desktop. */
    public static byte[] pbkdf2(byte[] passwordUtf8, byte[] salt, int iterations, int length)
            throws GeneralSecurityException {
        char[] chars = new String(passwordUtf8, StandardCharsets.UTF_8).toCharArray();
        PBEKeySpec spec = new PBEKeySpec(chars, salt, iterations, length * 8);
        try {
            return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
        } finally {
            spec.clearPassword();
            Arrays.fill(chars, '\0');
            Arrays.fill(passwordUtf8, (byte) 0);
        }
    }

    /** AES-256-GCM with a 12-byte nonce and 16-byte tag; returns ciphertext followed by the tag. */
    public static byte[] gcmEncrypt(byte[] key, byte[] nonce, byte[] aad, byte[] plaintext)
            throws GeneralSecurityException {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
        if (aad.length > 0) cipher.updateAAD(aad);
        try {
            return cipher.doFinal(plaintext);
        } finally {
            Arrays.fill(key, (byte) 0);
            Arrays.fill(plaintext, (byte) 0);
        }
    }

    /** Returns null when authentication fails (wrong key, or data altered). */
    public static byte[] gcmDecrypt(byte[] key, byte[] nonce, byte[] aad, byte[] ciphertextAndTag)
            throws GeneralSecurityException {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, nonce));
        if (aad.length > 0) cipher.updateAAD(aad);
        try {
            return cipher.doFinal(ciphertextAndTag);
        } catch (AEADBadTagException e) {
            return null;
        } finally {
            Arrays.fill(key, (byte) 0);
        }
    }
}
