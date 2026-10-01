cmake_minimum_required(VERSION 3.22)
project(openbooks_android LANGUAGES CXX)

# The engine is shared with the desktop apps: build it straight from the repository sources.
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

add_library(openbooks SHARED
    jni_bridge.cpp
    crypto_android.cpp
    ${REPO}/src/util.cpp
    ${REPO}/src/money.cpp
    ${REPO}/src/date.cpp
    ${REPO}/src/model.cpp
    ${REPO}/src/book.cpp
    ${REPO}/src/storage.cpp
    ${REPO}/src/import.cpp
    ${REPO}/src/crypto.cpp
    ${REPO}/src/console.cpp
    ${REPO}/src/pdf.cpp
    ${REPO}/src/invoice_pdf.cpp
    ${REPO}/src/reports.cpp
    ${REPO}/src/cli.cpp
)
target_include_directories(openbooks PRIVATE ${REPO}/include ${REPO}/src)
target_compile_features(openbooks PRIVATE cxx_std_17)
target_compile_options(openbooks PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
# 16 KB page alignment, required for apps on Android 15+ devices.
target_link_options(openbooks PRIVATE "-Wl,-z,max-page-size=16384")
