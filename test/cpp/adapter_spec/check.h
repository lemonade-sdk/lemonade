#pragma once
// A tiny harness shared by the adapter_spec job tests: CHECK and THROWS count failures; main returns
// the count so run.sh exits non-zero when anything failed.
#include <exception>
#include <iostream>
#include <string>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        bool ok_ = false;                                                              \
        try { ok_ = static_cast<bool>(cond); }                                         \
        catch (const std::exception& e_) { std::cerr << "  threw: " << e_.what() << "\n"; } \
        if (!ok_) { ++g_failures; std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; } \
    } while (0)

// expr must throw a SpecError whose message contains `needle` ("" = any message).
#define THROWS(expr, needle)                                                           \
    do {                                                                               \
        ++g_checks;                                                                    \
        bool threw_ = false;                                                           \
        std::string msg_;                                                              \
        try { (void)(expr); }                                                          \
        catch (const lemon::backends::adapter::specs::SpecError& e_) { threw_ = true; msg_ = e_.what(); } \
        catch (const std::exception& e_) { msg_ = std::string("wrong exception: ") + e_.what(); } \
        if (!threw_ || msg_.find(needle) == std::string::npos) {                       \
            ++g_failures;                                                              \
            std::cerr << "FAIL " << __LINE__ << ": " #expr " should throw SpecError containing \"" \
                      << (needle) << "\"; got: " << (threw_ ? msg_ : (msg_.empty() ? "no throw" : msg_)) << "\n"; \
        }                                                                              \
    } while (0)

inline int report(const char* job) {
    std::cout << job << ": " << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
