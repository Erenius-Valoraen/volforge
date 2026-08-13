// A deliberately tiny test harness.
//
// The dependency policy in cpp/README.md applies to test code too: a single
// header worth of registration and assertion macros is cheaper than a vendored
// framework, and swapping to one later is mechanical.

#pragma once

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace vftest {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) return;
    ++failures();
    std::printf("    FAIL  %s:%d  %s\n", file, line, expr);
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* expr, const char* file, int line) {
    if (a == b) return;
    ++failures();
    std::printf("    FAIL  %s:%d  %s\n", file, line, expr);
}

inline int run_all() {
    int failed_cases = 0;
    for (const auto& tc : registry()) {
        const int before = failures();
        // stderr is unbuffered, so a crash still names the test that caused it.
        std::fprintf(stderr, "  %s\n", tc.name);
        try {
            tc.fn();
        } catch (const std::exception& e) {
            ++failures();
            std::printf("    FAIL  threw: %s\n", e.what());
        } catch (...) {
            ++failures();
            std::printf("    FAIL  threw unknown exception\n");
        }
        if (failures() != before) ++failed_cases;
    }
    std::printf("\n%zu tests, %d failing checks, %d failing tests\n",
                registry().size(), failures(), failed_cases);
    return failures() == 0 ? 0 : 1;
}

}  // namespace vftest

#define VF_CONCAT_(a, b) a##b
#define VF_CONCAT(a, b) VF_CONCAT_(a, b)

#define TEST(name)                                                    \
    static void name();                                               \
    static ::vftest::Register VF_CONCAT(reg_, name){#name, name};     \
    static void name()

#define CHECK(expr) ::vftest::check((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::vftest::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr)                                            \
    do {                                                              \
        bool threw = false;                                           \
        try { (void)(expr); } catch (...) { threw = true; }           \
        ::vftest::check(threw, "throws: " #expr, __FILE__, __LINE__); \
    } while (false)
