// Regression coverage for llamacpp SYCL default-backend selection when the
// configured backend has no user-provided binary.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

#include <lemon/config_file.h>
#include <lemon/runtime_config.h>
#include <lemon/system_info.h>

namespace fs = std::filesystem;
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

#ifdef __linux__

class ScopedRuntimeConfig {
public:
    explicit ScopedRuntimeConfig(lemon::RuntimeConfig* config)
        : previous_(lemon::RuntimeConfig::global()) {
        lemon::RuntimeConfig::set_global(config);
    }

    ~ScopedRuntimeConfig() {
        lemon::RuntimeConfig::set_global(previous_);
    }

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

std::string test_cpu_family() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x86_64";
#endif
}

json test_devices_with_intel_gpu() {
    return {
        {"cpu",
         {
             {"name", "Test CPU"},
             {"available", true},
             {"family", test_cpu_family()},
         }},
        {"intel_gpu",
         json::array({
             {
                 {"name", "Intel Arc Pro B70"},
                 {"available", true},
                 {"family", "xe"},
                 {"device_id", "0xe223"},
                 {"pci", "0000:03:00.0"},
             },
         })},
    };
}

json build_recipes(TestSystemInfo& system_info, const std::string& configured_backend) {
    json config_json = lemon::ConfigFile::base_defaults();
    config_json["llamacpp"]["backend"] = configured_backend;
    lemon::RuntimeConfig config(config_json);
    ScopedRuntimeConfig config_scope(&config);
    return system_info.build_recipes_info(test_devices_with_intel_gpu());
}

#endif  // __linux__

}  // namespace

int main() {
#ifndef __linux__
    std::cout << "llamacpp SYCL default-backend tests are Linux-only; skipped."
              << std::endl;
    return 0;
#else
    TestSystemInfo system_info;
    const json recipes = build_recipes(system_info, "sycl");

    check(recipes.contains("llamacpp"), "llamacpp recipe exists");
    if (!recipes.contains("llamacpp")) {
        return failures == 0 ? 0 : 1;
    }

    const auto& backends = recipes["llamacpp"]["backends"];
    check(backends.contains("sycl"), "sycl backend is surfaced");
    if (backends.contains("sycl")) {
        check(backends["sycl"].value("state", "") == "not_installed",
              "missing SYCL binary reports not_installed");
    }

    const std::string default_backend =
        recipes["llamacpp"].value("default_backend", "");
    check(default_backend != "sycl",
          "configured sycl without binary does not become default_backend");
    check(!default_backend.empty(),
          "default_backend falls back when configured sycl is unavailable");

    const auto supported = lemon::SystemInfo::get_supported_backends("llamacpp");
    const bool sycl_listed =
        std::find(supported.backends.begin(), supported.backends.end(), "sycl")
        != supported.backends.end();
    check(!sycl_listed,
          "get_supported_backends omits not_installed sycl");

    if (failures != 0) {
        std::cerr << "Total failures: " << failures << std::endl;
        return 1;
    }

    std::cout << "All llamacpp SYCL default-backend tests passed!" << std::endl;
    return 0;
#endif
}
