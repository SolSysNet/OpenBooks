# OpenBooks for Android

A native Android app (Kotlin + Jetpack Compose) on top of the same C++ engine as the desktop app
and CLI. Books files are fully compatible in both directions, including password-protected ones.

## Features

Everything the desktop app does:

- **Dashboard:** bank balance, receivables and overdue invoices, payables, net income this year,
  recent activity, and a prompt when recurring invoices are due.
- **Sales:** customers, invoices, estimates (accept/decline, convert to an invoice), sales
  receipts, credit memos applied to invoices, recurring invoice templates, and payments
  applied to specific invoices (with auto-apply).
- **Purchases:** vendors, bills and bill payments.
- **Banking:** registers with running and cleared balances; quick expense, deposit and transfer
  entry; recategorize and void; reconciliation against a statement; journal entries with a
  live balance check; and CSV statement import with a preview.
- **Setup and reports:** chart of accounts, products and services, company settings (closing
  date, system accounts), P&L, Balance Sheet, Trial Balance, A/R and A/P aging, Journal and
  Account Detail. Reports can be shared or saved as CSV.
- **Files:** invoice PDFs (share, view or save), import and export `.obk` files, and optional
  password protection.

## Security on Android

- **No `INTERNET` permission.** Android itself blocks every network connection from the app.
- Books live in app-private storage. They are excluded from cloud backup and device-to-device
  transfer (`allowBackup=false` plus data-extraction rules). Use *Export books file* for backups.
- Password-protected books use the same format as the desktop:
  - PBKDF2-HMAC-SHA256 (600,000 iterations) + AES-256-GCM;
  - the primitives come from the platform's `javax.crypto` provider;
  - the engine runs known-answer tests (RFC 7914, McGrew–Viega GCM, a non-ASCII password vector)
    at startup and refuses to use encryption if they fail.
- While password-protected books are open:
  - screenshots and the recent-apps thumbnail are blocked (`FLAG_SECURE`);
  - the books lock after 5 minutes in the background.
- PDFs and CSVs are written to a private cache folder and shared only through a `FileProvider`
  limited to that folder. The folder is emptied when books are closed.

## Building

Requirements (Android Studio's SDK Manager): Android SDK Platform 37, NDK 30.0.16248370 and
CMake 4.1.2. JDK 17+ (Android Studio's bundled JBR works).

```bash
cd android
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew assembleRelease      # minified with R8; sign it with your own key
```

Or open the `android/` folder in Android Studio.

### Dependency pinning

Unlike the C++ build, Gradle downloads the Android build tools and libraries (Android Gradle
Plugin, Kotlin, Jetpack Compose). They are pinned:

- `gradle/wrapper/gradle-wrapper.properties` pins the Gradle distribution by SHA-256
  (`distributionSha256Sum`). The committed `gradle-wrapper.jar` matches Gradle's published checksum.
- `gradle/verification-metadata.xml` lists the SHA-256 of **every** artifact the build uses.
  Gradle refuses to build if any download doesn't match.
- Repositories are limited to Google's Maven (only `com.android`, `com.google` and `androidx`
  groups) and Maven Central. Modules cannot add their own repositories.

When you change a version in `gradle/libs.versions.toml`, regenerate the checksums and review
the diff of `verification-metadata.xml` in the same commit:

```bash
./gradlew --write-verification-metadata sha256 help assembleDebug assembleRelease
```

## How it fits together

```
Compose UI (Kotlin)  ──JSON──▶  NativeBooks.callBytes (JNI)  ──▶  jni_bridge.cpp  ──▶  openbooks engine (../src)
                                                                  crypto_android.cpp ──JNI──▶ NativeCrypto (javax.crypto)
```

- `app/src/main/cpp/jni_bridge.cpp` exposes the engine as ~50 JSON operations (`status`,
  `open`, `createDocument`, `report`, …).
  - Money travels as exact decimal strings. Floating-point numbers are rejected.
  - Every change is applied to a copy of the books, saved, and only then swapped in, as on the desktop.
- `app/src/main/cpp/crypto_android.cpp` implements the engine's `ob::crypto` interface with
  `javax.crypto`, the Android counterpart of `crypto_cng.cpp` and `crypto_openssl.cpp`.
- `app/src/main/java/org/openbooks/android/` holds the Compose UI:
  - `Core.kt`: typed calls;
  - `Shell.kt`: navigation drawer and welcome screen;
  - `SalesScreens.kt`, `BankingScreens.kt` and `SetupScreens.kt`: the screens.

### Testing the bridge without a device

The bridge doesn't depend on Android, only on JNI. It can be compiled as a desktop shared
library and driven from a desktop JVM, with the JDK's `javax.crypto` standing in for Android's.
Build it with MinGW using the NDK's `jni.h` (copy only `jni.h` into an include folder of its own):

```bash
g++ -std=c++17 -shared -o openbooks.dll -I include -I src -isystem <folder-with-jni.h> \
    android/app/src/main/cpp/jni_bridge.cpp android/app/src/main/cpp/crypto_android.cpp src/*.cpp -static
javac -d classes android/app/src/main/java/org/openbooks/core/*.java YourHarness.java
java -Djava.library.path=. -cp classes YourHarness
```

Leave out `src/main.cpp`, `src/crypto_cng.cpp` and `src/crypto_openssl.cpp`; see
`app/src/main/cpp/CMakeLists.txt` for the exact source list.
