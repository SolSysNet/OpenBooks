package org.openbooks.core;

import java.nio.charset.StandardCharsets;

/**
 * The single entry point into the native OpenBooks engine (jni_bridge.cpp).
 * Requests and replies are JSON; text crosses JNI as UTF-8 bytes (not JNI's "modified
 * UTF-8"), so every character, including emoji, round-trips exactly.
 */
public final class NativeBooks {
    static {
        System.loadLibrary("openbooks");
    }

    private NativeBooks() {}

    private static native byte[] callBytes(byte[] requestUtf8);

    public static String call(String requestJson) {
        byte[] reply = callBytes(requestJson.getBytes(StandardCharsets.UTF_8));
        return new String(reply, StandardCharsets.UTF_8);
    }
}
