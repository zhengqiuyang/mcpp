// Minimal dependency-free test harness shared by all mcpp tests.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

namespace mcpptest {

inline int g_failures = 0;
inline int g_checks = 0;

inline void report_eq(const char* file, int line, const char* expr,
                      const std::string& got, const std::string& want) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n  got:  %s\n  want: %s\n", file, line, expr, got.c_str(), want.c_str());
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++mcpptest::g_checks;                                              \
        if (!(cond)) {                                                     \
            ++mcpptest::g_failures;                                        \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        ++mcpptest::g_checks;                                              \
        auto va_ = (a);                                                    \
        auto vb_ = (b);                                                    \
        if (!(va_ == vb_)) {                                               \
            std::string sa_, sb_;                                          \
            mcpptest::to_string_impl(sa_, va_);                            \
            mcpptest::to_string_impl(sb_, vb_);                            \
            mcpptest::report_eq(__FILE__, __LINE__, #a " == " #b, sa_, sb_); \
        }                                                                  \
    } while (0)

// Cheap stringification for the common test types.
template <typename T>
inline void to_string_impl(std::string& out, const T& v) {
    if constexpr (std::is_same_v<T, std::string>) {
        out = v;
    } else if constexpr (std::is_same_v<T, bool>) {
        out = v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        out = std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", v);
        out = buf;
    } else {
        out = std::to_string(v);
    }
}

}  // namespace mcpptest
