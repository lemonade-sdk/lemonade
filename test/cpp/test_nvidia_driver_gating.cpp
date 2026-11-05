// Recipe-table coverage for NVIDIA driver gating: a CUDA backend must not be
// offered when the NVIDIA driver is missing, and llamacpp:cuda must not be the
// auto default when the driver predates its CUDA major version.
//
// Build: cmake --build --preset default --target test_nvidia_driver_gating
// Run:   ctest --test-dir build -R '^NvidiaDriverGatingTest$' --output-on-failure

#include <iostream>
#include <string>

#include <lemon/config_file.h>
#include <lemon/runtime_config.h>
#include <lemon/system_info.h>

using lemon::json;

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "[ok] " << message << std::endl;
    } else {
        std::cerr << "[FAIL] " << message << std::endl;
        ++failures;
    }
}

#if defined(_WIN32) || defined(__linux__)

class ScopedRuntimeConfig {
public:
    explicit ScopedRuntimeConfig(lemon::RuntimeConfig* config)
        : previous_(lemon::RuntimeConfig::global()) {
        lemon::RuntimeConfig::set_global(config);
    }
    ~ScopedRuntimeConfig() { lemon::RuntimeConfig::set_global(previous_); }

private:
    lemon::RuntimeConfig* previous_;
};

class TestSystemInfo final : public lemon::SystemInfo {
public:
    lemon::CPUInfo get_cpu_device() override { return {}; }
    lemon::GPUInfo get_amd_igpu_device() override { return {}; }
    std::vector<lemon::GPUInfo> get_amd_dgpu_devices() override { return {}; }
    std::vector<lemon::GPUInfo> get_nvidia_gpu_devices() override { return {}; }
    lemon::NPUInfo get_npu_device() override { return {}; }
};

#ifdef _WIN32
const char* kOldDriver = "516.94";
const char* kCurrentDriver = "560.94";
#else
const char* kOldDriver = "515.105.01";
const char* kCurrentDriver = "550.163.01";
#endif

std::string cpu_family() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "x86_64";
#endif
}

json devices_with_nvidia(json gpu) {
    return {
        {"cpu", {{"name", "Test CPU"}, {"available", true}, {"family", cpu_family()}}},
        {"nvidia_gpu", json::array({gpu})},
    };
}

json rtx_4090(const std::string& driver_version, bool driver_missing) {
    json gpu = {
        {"name", "NVIDIA GeForce RTX 4090"},
        {"available", true},
        {"family", "sm_89"},
        {"driver_version", driver_version},
    };
    if (driver_missing) {
        gpu["driver_missing"] = true;
    }
    return gpu;
}

json build_recipes(const json& devices) {
    json config_json = lemon::ConfigFile::base_defaults();
    config_json["llamacpp"]["prefer_system"] = false;
    lemon::RuntimeConfig config(config_json);
    ScopedRuntimeConfig scope(&config);
    TestSystemInfo system_info;
    return system_info.build_recipes_info(devices);
}

json cuda_status(const json& recipes) {
    return recipes["llamacpp"]["backends"].value("cuda", json::object());
}

std::string default_backend(const json& recipes) {
    return recipes["llamacpp"].value("default_backend", "");
}

void test_current_driver_is_not_gated() {
    const json recipes = build_recipes(devices_with_nvidia(rtx_4090(kCurrentDriver, false)));
    const std::string state = cuda_status(recipes).value("state", "");
    check(state != "unsupported" && state != "action_required",
          "current driver: cuda is offered (state=" + state + ")");
}

void test_missing_driver_is_unsupported() {
    const json recipes = build_recipes(devices_with_nvidia(rtx_4090("Unknown", true)));
    const json cuda = cuda_status(recipes);
    check(cuda.value("state", "") == "unsupported", "missing driver: cuda is unsupported");
    check(cuda.value("message", "").find("driver is not installed") != std::string::npos,
          "missing driver: message names the missing driver");
    check(default_backend(recipes) != "cuda", "missing driver: cuda is not the auto default");
}

void test_old_driver_requires_action() {
    const json recipes = build_recipes(devices_with_nvidia(rtx_4090(kOldDriver, false)));
    const json cuda = cuda_status(recipes);
    check(cuda.value("state", "") == "action_required", "old driver: cuda is action_required");
    check(cuda.value("message", "").find(kOldDriver) != std::string::npos,
          "old driver: message names the installed driver");
    check(cuda.value("action", "").rfind("https://", 0) == 0,
          "old driver: action is a driver download link");
    check(default_backend(recipes) != "cuda", "old driver: cuda is not the auto default");
}

void test_unreadable_driver_version_is_not_gated() {
    const json recipes = build_recipes(devices_with_nvidia(rtx_4090("Unknown", false)));
    const std::string state = cuda_status(recipes).value("state", "");
    check(state != "unsupported" && state != "action_required",
          "unreadable driver version: cuda is not blocked (state=" + state + ")");
}

#endif

}  // namespace

int main() {
#if defined(_WIN32) || defined(__linux__)
    test_current_driver_is_not_gated();
    test_missing_driver_is_unsupported();
    test_old_driver_requires_action();
    test_unreadable_driver_version_is_not_gated();
#else
    std::cout << "[skip] CUDA backends are only offered on Windows and Linux" << std::endl;
#endif
    if (failures != 0) {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    return 0;
}
