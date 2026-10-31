#include <cstdlib>
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

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name), previous_(std::getenv(name) ? std::getenv(name) : ""),
          had_previous_(std::getenv(name) != nullptr) {}

    ~ScopedEnvVar() {
        restore();
    }

    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    void clear() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

private:
    void restore() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::string previous_;
    bool had_previous_;
};

}  // namespace

int main() {
    ScopedEnvVar rocr_visible_devices("ROCR_VISIBLE_DEVICES");
    ScopedEnvVar hip_visible_devices("HIP_VISIBLE_DEVICES");
    ScopedEnvVar cuda_visible_devices("CUDA_VISIBLE_DEVICES");
    ScopedEnvVar gpu_device_ordinal("GPU_DEVICE_ORDINAL");
    rocr_visible_devices.clear();
    hip_visible_devices.clear();
    cuda_visible_devices.clear();
    gpu_device_ordinal.clear();
    check(!lemon::existing_rocm_visibility_override_env_name(),
          "the production selector reports no inherited visibility mask when none is set");

    rocr_visible_devices.set("");
    cuda_visible_devices.set("2");
    gpu_device_ordinal.set("3");
#ifdef _WIN32
    check(!lemon::existing_rocm_visibility_override_env_name(),
          "Windows ignores non-HIP inherited visibility masks");
    hip_visible_devices.set("1");
    check(is(lemon::existing_rocm_visibility_override_env_name(), "HIP_VISIBLE_DEVICES"),
          "the production selector honors HIP_VISIBLE_DEVICES on Windows");
#else
    hip_visible_devices.set("1");
    check(is(lemon::existing_rocm_visibility_override_env_name(), "ROCR_VISIBLE_DEVICES"),
          "the production selector prioritizes ROCR_VISIBLE_DEVICES");
    rocr_visible_devices.clear();
    check(is(lemon::existing_rocm_visibility_override_env_name(), "HIP_VISIBLE_DEVICES"),
          "the production selector falls back to HIP_VISIBLE_DEVICES");
    hip_visible_devices.clear();
    check(is(lemon::existing_rocm_visibility_override_env_name(), "CUDA_VISIBLE_DEVICES"),
          "the production selector detects CUDA_VISIBLE_DEVICES");
    cuda_visible_devices.clear();
    check(is(lemon::existing_rocm_visibility_override_env_name(), "GPU_DEVICE_ORDINAL"),
          "the production selector detects GPU_DEVICE_ORDINAL");
#endif

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
    check(!lemon::should_apply_rocm_device_arch_override(
              "ROCm999", "", "HIP_VISIBLE_DEVICES"),
          "a caller-owned visibility mask skips architecture lookup for unavailable ROCm indices");
    check(!lemon::should_apply_rocm_device_arch_override(
              "ROCm-invalid", "", "HIP_VISIBLE_DEVICES"),
          "a caller-owned visibility mask leaves invalid ROCm selections to the runtime");
#ifndef _WIN32
    check(!lemon::should_apply_rocm_device_arch_override(
              "ROCm0", "", "CUDA_VISIBLE_DEVICES"),
          "an inherited CUDA mask prevents a conflicting ROCm architecture override");
    check(!lemon::should_apply_rocm_device_arch_override(
              "ROCm0", "", "GPU_DEVICE_ORDINAL"),
          "an inherited GPU ordinal mask prevents a conflicting ROCm architecture override");
#endif
    check(!lemon::should_apply_rocm_device_arch_override(
              "ROCm0", "gfx1100", nullptr),
          "an explicit architecture override takes precedence over device-based resolution");
    check(lemon::should_apply_rocm_device_arch_override("ROCm0", "", nullptr),
          "an explicit ROCm device is resolved when no caller override is present");
    return failures == 0 ? 0 : 1;
}
