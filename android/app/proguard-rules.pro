# The native engine calls these classes and methods by name over JNI.
-keep class org.openbooks.core.NativeCrypto { public static *; }
-keep class org.openbooks.core.NativeBooks { native <methods>; }
