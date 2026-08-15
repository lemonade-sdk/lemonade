#include <lemon/runtime_config.h>

#include <cstdio>
#include <stdexcept>
#include <string>

using lemon::RuntimeConfig;

int main() {
    int failures = 0;

    try {
        RuntimeConfig::validate_backend_choice("llamacpp", "system");
#ifdef __linux__
        std::puts("[PASS] llamacpp system backend is accepted on Linux");
#else
        std::puts("[FAIL] llamacpp system backend was accepted on an unsupported platform");
        ++failures;
#endif
    } catch (const std::invalid_argument& error) {
#ifdef __linux__
        std::printf("[FAIL] llamacpp system backend was rejected on Linux: %s\n", error.what());
        ++failures;
#else
        std::puts("[PASS] llamacpp system backend is rejected on unsupported platforms");
#endif
    }

    return failures == 0 ? 0 : 1;
}
