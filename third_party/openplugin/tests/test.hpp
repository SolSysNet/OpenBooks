#pragma once

// The test framework shared by the test files (no external framework needed), plus small helpers.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace testing {

inline std::ostream& operator<<(std::ostream& os, const std::vector<std::string>& v) {
    os << "[";
    for (std::size_t i = 0; i < v.size(); ++i) os << (i ? ", " : "") << '"' << v[i] << '"';
    return os << "]";
}

inline int g_checks = 0;
inline int g_failures = 0;

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

#define TEST(name)                                   \
    void name();                                     \
    const Registrar registrar_##name(#name, name);   \
    void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++testing::g_checks;                                                                        \
        if (!(cond)) {                                                                     \
            ++testing::g_failures;                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";     \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++testing::g_checks;                                                                                        \
        const auto va = (a);                                                                               \
        const auto vb = (b);                                                                               \
        if (!(va == vb)) {                                                                                 \
            ++testing::g_failures;                                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b "\n"            \
                      << "    left:  " << va << "\n    right: " << vb << "\n";                             \
        }                                                                                                  \
    } while (0)

// Checks that `expr` throws ExType and that its message contains `fragment`.
#define CHECK_THROWS(expr, ExType, fragment)                                                               \
    do {                                                                                                   \
        ++testing::g_checks;                                                                                        \
        bool threw = false;                                                                                \
        try {                                                                                              \
            (void)(expr);                                                                                  \
        } catch (const ExType& e) {                                                                        \
            threw = true;                                                                                  \
            if (std::string(e.what()).find(fragment) == std::string::npos) {                              \
                ++testing::g_failures;                                                                              \
                std::cerr << __FILE__ << ":" << __LINE__ << ": wrong message for " #expr ": " << e.what()  \
                          << "\n    expected to contain: " << fragment << "\n";                            \
            }                                                                                              \
        } catch (const std::exception& e) {                                                                \
            threw = true;                                                                                  \
            ++testing::g_failures;                                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #expr " threw the wrong type: " << e.what()   \
                      << "\n";                                                                             \
        }                                                                                                  \
        if (!threw) {                                                                                      \
            ++testing::g_failures;                                                                                  \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected " #expr " to throw\n";                 \
        }                                                                                                  \
    } while (0)

// ------------------------------------------------------------------ helpers

inline int processId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

// A fresh, empty temporary folder, removed when the test ends.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("openplugin_test_" + std::to_string(processId()) + "_" + std::to_string(++counter));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

inline void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

inline void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace testing

using testing::operator<<;
