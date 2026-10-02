cmake_minimum_required(VERSION 3.16)
project(OpenPlugin VERSION 0.1.0 LANGUAGES CXX)

# openplugin is vendored into OpenBooks, OpenTax and OpenPractice (third_party/openplugin) and added
# with add_subdirectory. Tests build by default only when this is the top-level project.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(OPENPLUGIN_TOP_LEVEL ON)
else()
    set(OPENPLUGIN_TOP_LEVEL OFF)
endif()
option(OPENPLUGIN_BUILD_TESTS "Build the openplugin test suite" ${OPENPLUGIN_TOP_LEVEL})
# The runner is the only part of openplugin (and of the apps) with network code. Turn it off for a
# build that contains none; Lua plugins then show as "runner not installed".
option(OPENPLUGIN_BUILD_RUNNER "Build openplugin-runner, which runs Lua plugins" ON)

if(OPENPLUGIN_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Warnings for openplugin's own code.
add_library(openplugin_warnings INTERFACE)
if(MSVC)
    target_compile_options(openplugin_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(openplugin_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------------ host library
# Everything the apps link: JSON, hashing, child processes, JSON-RPC, manifests, the registry of
# enabled plugins and per-file plugin data. No network code.
add_library(openplugin
    src/json.cpp
    src/sha256.cpp
    src/process.cpp
    src/rpc.cpp
    src/manifest.cpp
    src/registry.cpp
    src/store.cpp
)
target_include_directories(openplugin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(openplugin PUBLIC cxx_std_17)
target_link_libraries(openplugin PRIVATE openplugin_warnings)
find_package(Threads REQUIRED)
target_link_libraries(openplugin PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(openplugin PUBLIC /utf-8)
    target_compile_definitions(openplugin PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ----------------------------------------------------------------------- runner
if(OPENPLUGIN_BUILD_RUNNER)
    # Lua 5.4.9, vendored unmodified (third_party/lua-5.4.9/README.openplugin.md). Only the core and
    # the base, coroutine, math, string, table and utf8 libraries are built: io, os, package, debug
    # and linit.c are left out, so plugins can't reach them even by accident.
    #
    # Compiled as C++ so Lua errors unwind with C++ exceptions: destructors in the runner's own code
    # run when a plugin error passes through it.
    set(LUA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.4.9/src)
    set(OPENPLUGIN_LUA_SOURCES)
    foreach(f lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
              lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
        list(APPEND OPENPLUGIN_LUA_SOURCES ${LUA_SRC}/${f}.c)
    endforeach()
    set_source_files_properties(${OPENPLUGIN_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
    add_library(openplugin_lua STATIC ${OPENPLUGIN_LUA_SOURCES})
    target_include_directories(openplugin_lua PUBLIC ${LUA_SRC})
    target_compile_features(openplugin_lua PUBLIC cxx_std_17)
    if(MSVC)
        target_compile_options(openplugin_lua PRIVATE /W0)
    else()
        target_compile_options(openplugin_lua PRIVATE -w $<$<CXX_COMPILER_ID:Clang,AppleClang>:-xc++>)
    endif()
    if(UNIX)
        target_link_libraries(openplugin_lua PUBLIC m)
    endif()

    add_library(openplugin_runner_core STATIC
        runner/codec.cpp
        runner/http.cpp
        runner/lua_json.cpp
        runner/runner.cpp
    )
    target_include_directories(openplugin_runner_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runner)
    target_link_libraries(openplugin_runner_core PUBLIC openplugin openplugin_lua PRIVATE openplugin_warnings)
    if(WIN32)
        # WinHTTP and CNG's random number generator: both part of Windows.
        target_sources(openplugin_runner_core PRIVATE runner/http_winhttp.cpp)
        target_link_libraries(openplugin_runner_core PUBLIC winhttp bcrypt)
    else()
        find_package(CURL)
        if(NOT CURL_FOUND)
            message(FATAL_ERROR
                "openplugin-runner needs the system libcurl for HTTPS (or configure with -DOPENPLUGIN_BUILD_RUNNER=OFF).\n"
                "  Debian/Ubuntu: sudo apt install libcurl4-openssl-dev\n"
                "  Fedora:        sudo dnf install libcurl-devel\n"
                "  macOS:         preinstalled with the Xcode command line tools")
        endif()
        target_sources(openplugin_runner_core PRIVATE runner/http_curl.cpp)
        target_link_libraries(openplugin_runner_core PUBLIC CURL::libcurl)
    endif()

    add_executable(openplugin-runner runner/main.cpp)
    target_link_libraries(openplugin-runner PRIVATE openplugin_runner_core openplugin_warnings)
    if(WIN32)
        target_link_libraries(openplugin-runner PRIVATE shell32)  # CommandLineToArgvW
    endif()
    if(MINGW)
        target_link_options(openplugin-runner PRIVATE -static)
    endif()
    install(TARGETS openplugin-runner RUNTIME DESTINATION bin)
endif()

# ------------------------------------------------------------------------ tests
if(OPENPLUGIN_BUILD_TESTS)
    enable_testing()

    # A stand-in plugin process the tests drive: echoes, misbehaves, hangs or crashes on request.
    add_executable(openplugin_test_child tests/test_child.cpp)
    target_link_libraries(openplugin_test_child PRIVATE openplugin openplugin_warnings)
    if(WIN32)
        target_link_libraries(openplugin_test_child PRIVATE shell32)  # CommandLineToArgvW
    endif()

    add_executable(openplugin_tests tests/test_main.cpp)
    target_link_libraries(openplugin_tests PRIVATE openplugin openplugin_warnings)
    target_compile_definitions(openplugin_tests PRIVATE
        OPENPLUGIN_TEST_CHILD="$<TARGET_FILE:openplugin_test_child>")
    add_dependencies(openplugin_tests openplugin_test_child)
    if(OPENPLUGIN_BUILD_RUNNER)
        # The runner's codecs and HTTP policy directly, and the runner itself end to end.
        target_sources(openplugin_tests PRIVATE tests/test_runner.cpp)
        target_link_libraries(openplugin_tests PRIVATE openplugin_runner_core)
        target_compile_definitions(openplugin_tests PRIVATE OPENPLUGIN_RUNNER="$<TARGET_FILE:openplugin-runner>")
        add_dependencies(openplugin_tests openplugin-runner)
        if(WIN32)
            target_link_libraries(openplugin_tests PRIVATE ws2_32)  # the local test HTTP server
        endif()
    endif()
    if(MINGW)
        target_link_options(openplugin_tests PRIVATE -static)
        target_link_options(openplugin_test_child PRIVATE -static)
    endif()
    add_test(NAME openplugin_tests COMMAND openplugin_tests)
endif()
