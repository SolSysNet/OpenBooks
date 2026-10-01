cmake_minimum_required(VERSION 3.16)
project(OpenBooks VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENBOOKS_BUILD_TESTS "Build the OpenBooks test suite" ON)
option(OPENBOOKS_BUILD_GUI "Build the OpenBooks desktop app (Dear ImGui)" ON)

# Warnings for OpenBooks' own code (not applied to third-party sources).
add_library(openbooks_warnings INTERFACE)
if(MSVC)
    target_compile_options(openbooks_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(openbooks_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------------ core engine
add_library(openbooks_core
    src/util.cpp
    src/money.cpp
    src/date.cpp
    src/model.cpp
    src/book.cpp
    src/storage.cpp
    src/import.cpp
    src/crypto.cpp
    src/console.cpp
    src/pdf.cpp
    src/invoice_pdf.cpp
    src/reports.cpp
    src/cli.cpp
)
target_include_directories(openbooks_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Encryption primitives come from the platform, never from a download:
#   Windows: CNG (bcrypt.dll, part of Windows)
#   Linux/macOS: the system's preinstalled OpenSSL libcrypto (hard requirement)
if(WIN32)
    target_sources(openbooks_core PRIVATE src/crypto_cng.cpp)
    target_link_libraries(openbooks_core PUBLIC bcrypt)
else()
    find_package(OpenSSL 1.1.1 COMPONENTS Crypto)
    if(NOT OpenSSL_FOUND)
        message(FATAL_ERROR
            "OpenBooks needs a preinstalled OpenSSL (libcrypto, 1.1.1 or newer) for file encryption.\n"
            "  Debian/Ubuntu: sudo apt install libssl-dev\n"
            "  Fedora:        sudo dnf install openssl-devel\n"
            "  macOS:         brew install openssl@3, then pass -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)")
    endif()
    target_sources(openbooks_core PRIVATE src/crypto_openssl.cpp)
    target_link_libraries(openbooks_core PUBLIC OpenSSL::Crypto)
endif()
target_link_libraries(openbooks_core PRIVATE openbooks_warnings)
if(MSVC)
    target_compile_options(openbooks_core PUBLIC /utf-8)
    target_compile_definitions(openbooks_core PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# Self-contained executables on MinGW (no libstdc++/libgcc DLLs to ship).
function(openbooks_static_runtime target)
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
    endif()
endfunction()

# -------------------------------------------------------------------------- CLI
add_executable(openbooks src/main.cpp)
target_link_libraries(openbooks PRIVATE openbooks_core openbooks_warnings)
if(WIN32)
    target_link_libraries(openbooks PRIVATE shell32)
endif()
openbooks_static_runtime(openbooks)
install(TARGETS openbooks RUNTIME DESTINATION bin)

# ------------------------------------------------------------------------ tests
if(OPENBOOKS_BUILD_TESTS)
    enable_testing()
    add_executable(openbooks_tests tests/test_main.cpp)
    target_link_libraries(openbooks_tests PRIVATE openbooks_core openbooks_warnings)
    openbooks_static_runtime(openbooks_tests)
    add_test(NAME openbooks_tests COMMAND openbooks_tests)
endif()

# -------------------------------------------------------------------------- GUI
if(OPENBOOKS_BUILD_GUI)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/misc/imgui_stdlib.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc)
    # Hardening: remove ImGui's built-in "open URL/file in shell" handler (ShellExecute / system())
    # so no widget can ever launch a URL or command, and drop deprecated APIs.
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

    set(OPENBOOKS_GUI_SOURCES
        gui/app.cpp
        gui/app_sales.cpp
        gui/app_documents.cpp
        gui/app_banking.cpp
        gui/app_setup.cpp
        gui/app_reports.cpp
        gui/widgets.cpp
        gui/theme.cpp
    )

    if(WIN32)
        target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
        target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)
        add_executable(openbooks-gui WIN32 ${OPENBOOKS_GUI_SOURCES} gui/main_win32_dx11.cpp gui/platform_win32.cpp)
        target_link_libraries(openbooks-gui PRIVATE comdlg32 shell32 ole32)
    else()
        find_package(glfw3 3.3 REQUIRED)
        find_package(OpenGL REQUIRED)
        target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
        target_link_libraries(imgui PUBLIC glfw OpenGL::GL ${CMAKE_DL_LIBS})
        add_executable(openbooks-gui ${OPENBOOKS_GUI_SOURCES} gui/main_glfw_opengl3.cpp gui/platform_posix.cpp)
    endif()
    target_link_libraries(openbooks-gui PRIVATE openbooks_core imgui openbooks_warnings)
    openbooks_static_runtime(openbooks-gui)
    install(TARGETS openbooks-gui RUNTIME DESTINATION bin)
endif()
