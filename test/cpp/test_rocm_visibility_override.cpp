#include <iostream>
#include <string>

#include <lemon/rocm_arch_override.h>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::cout << "[ok] " << message << std::endl;
    } else {
        std::cerr << "[FAIL] " << message << std::endl;
        ++failures;
    }
}

bool is(const char* actual, const char* expected) {
    return actual && std::string(actual) == expected;
}

}  // namespace

int main() {
#ifdef _WIN32
    check(is(lemon::rocm_visibility_override_env_name("", "1", "2", "3"),
             "HIP_VISIBLE_DEVICES"),
          "Windows selects HIP_VISIBLE_DEVICES when multiple masks are set");
    check(is(lemon::rocm_visibility_override_env_name(nullptr, "", "2", "3"),
             "HIP_VISIBLE_DEVICES"),
          "an empty HIP_VISIBLE_DEVICES value remains an explicit override");
    check(!lemon::rocm_visibility_override_env_name(nullptr, nullptr, "", "3"),
          "Windows ignores CUDA_VISIBLE_DEVICES as a ROCm override");
    check(!lemon::rocm_visibility_override_env_name(nullptr, nullptr, nullptr, ""),
          "Windows ignores GPU_DEVICE_ORDINAL as a ROCm override");
#else
    check(is(lemon::rocm_visibility_override_env_name("", "1", "2", "3"),
             "ROCR_VISIBLE_DEVICES"),
          "ROCR_VISIBLE_DEVICES has priority and accepts an empty value");
    check(is(lemon::rocm_visibility_override_env_name(nullptr, "", "2", "3"),
             "HIP_VISIBLE_DEVICES"),
          "HIP_VISIBLE_DEVICES has priority over compatibility masks");
    check(is(lemon::rocm_visibility_override_env_name(nullptr, nullptr, "", "3"),
             "CUDA_VISIBLE_DEVICES"),
          "an empty CUDA_VISIBLE_DEVICES value remains an explicit override");
    check(is(lemon::rocm_visibility_override_env_name(nullptr, nullptr, nullptr, ""),
             "GPU_DEVICE_ORDINAL"),
          "an empty GPU_DEVICE_ORDINAL value remains an explicit override");
#endif
    check(!lemon::rocm_visibility_override_env_name(nullptr, nullptr, nullptr, nullptr),
          "no override is reported when no visibility mask is set");
    return failures == 0 ? 0 : 1;
}
