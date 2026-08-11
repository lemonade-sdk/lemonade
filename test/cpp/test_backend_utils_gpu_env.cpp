// Unit tests for lemon::backends::BackendUtils::validate_device_backend_match()
// and lemon::backends::BackendUtils::apply_cuda_env_vars().
//
// These tests exercise environment variable override precedence and device
// selection validation deterministically without requiring physical GPU hardware.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <lemon/backends/backend_utils.h>
#include <lemon/runtime_config.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        std::cout << "[ok] " << msg << std::endl;
    } else {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), /*overwrite=*/1);
#endif
}

void clear_env_var(const std::string& name) {
#ifdef _WIN32
    _putenv((name + "=").c_str());
#else
    unsetenv(name.c_str());
#endif
}

bool has_key(const std::vector<std::pair<std::string, std::string>>& env_vars,
             const std::string& key) {
    for (const auto& pair : env_vars) {
        if (pair.first == key) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // Ensure clean host environment state before running tests
    clear_env_var("HIP_VISIBLE_DEVICES");
    clear_env_var("ROCR_VISIBLE_DEVICES");
    clear_env_var("CUDA_VISIBLE_DEVICES");

    // Test 1: apply_cuda_env_vars respects pre-existing CUDA_VISIBLE_DEVICES in host environment
    {
        set_env_var("CUDA_VISIBLE_DEVICES", "3");
        std::vector<std::pair<std::string, std::string>> env_vars;
        BackendUtils::apply_cuda_env_vars(env_vars, "TestCuda", /*skip_visible_devices=*/false);
        check(!has_key(env_vars, "CUDA_VISIBLE_DEVICES"),
              "apply_cuda_env_vars respects pre-existing CUDA_VISIBLE_DEVICES host override");
        clear_env_var("CUDA_VISIBLE_DEVICES");
    }

    // Test 2: validate_device_backend_match throws invalid_argument on device/backend mismatch
    {
        bool threw_rocm_vulkan = false;
        try {
            BackendUtils::validate_device_backend_match("vulkan", "ROCm2");
        } catch (const std::invalid_argument& e) {
            threw_rocm_vulkan = true;
        }
        check(threw_rocm_vulkan,
              "validate_device_backend_match throws invalid_argument for ROCm device on vulkan backend");

        bool threw_cuda_rocm = false;
        try {
            BackendUtils::validate_device_backend_match("rocm-stable", "CUDA0");
        } catch (const std::invalid_argument& e) {
            threw_cuda_rocm = true;
        }
        check(threw_cuda_rocm,
              "validate_device_backend_match throws invalid_argument for CUDA device on rocm backend");

        bool no_throw_matching = true;
        try {
            BackendUtils::validate_device_backend_match("vulkan", "Vulkan0");
            BackendUtils::validate_device_backend_match("rocm-stable", "ROCm1");
            BackendUtils::validate_device_backend_match("cuda", "CUDA2");
            BackendUtils::validate_device_backend_match("system", "ROCm2");
            BackendUtils::validate_device_backend_match("auto", "ROCm2");
            BackendUtils::validate_device_backend_match("", "ROCm1");
            BackendUtils::validate_device_backend_match("vulkan", "");
        } catch (...) {
            no_throw_matching = false;
        }
        check(no_throw_matching,
              "validate_device_backend_match accepts matching pairs and system/auto backends gracefully");
    }

    // Test 3: a custom backend binary environment variable takes precedence
    // over config.json. ROCm channel names collapse to the public ROCm override
    // documented for users, while config remains the fallback.
    {
        const std::string override_path = std::filesystem::current_path().string();
        const std::string config_path = std::filesystem::temp_directory_path().string();
        lemon::RuntimeConfig config(lemon::json{{"llamacpp", {{"rocm_bin", config_path}}}});
        lemon::RuntimeConfig::set_global(&config);
        set_env_var("LEMONADE_LLAMACPP_ROCM_BIN", override_path);

        check(BackendUtils::find_external_backend_binary("llamacpp", "rocm-stable") == override_path,
              "ROCm stable resolves LEMONADE_LLAMACPP_ROCM_BIN");
        check(BackendUtils::find_external_backend_binary("llamacpp", "rocm-nightly") == override_path,
              "ROCm nightly resolves LEMONADE_LLAMACPP_ROCM_BIN");
        check(BackendUtils::get_bin_config_value("llamacpp", "rocm-stable") == override_path,
              "raw bin resolution gives the environment override precedence over config");

        clear_env_var("LEMONADE_LLAMACPP_ROCM_BIN");
        check(BackendUtils::find_external_backend_binary("llamacpp", "rocm-stable") == config_path,
              "ROCm stable falls back to configured llamacpp.rocm_bin");
        lemon::RuntimeConfig::set_global(nullptr);
    }

    // Test 4: an environment path also overrides the reserved "builtin" config
    // value used by status and version resolution. Clearing the environment
    // restores the configured value and disables the external-binary path.
    {
        const std::string override_path = std::filesystem::current_path().string();
        lemon::RuntimeConfig config(lemon::json{{"llamacpp", {{"rocm_bin", "builtin"}}}});
        lemon::RuntimeConfig::set_global(&config);
        set_env_var("LEMONADE_LLAMACPP_ROCM_BIN", override_path);

        check(BackendUtils::get_bin_config_value("llamacpp", "rocm-stable") == override_path,
              "environment path overrides builtin for status and version resolution");
        check(BackendUtils::find_external_backend_binary("llamacpp", "rocm-stable") == override_path,
              "environment path overrides builtin for runtime binary resolution");

        clear_env_var("LEMONADE_LLAMACPP_ROCM_BIN");
        check(BackendUtils::get_bin_config_value("llamacpp", "rocm-stable") == "builtin",
              "raw bin resolution falls back to builtin after clearing the environment");
        check(BackendUtils::find_external_backend_binary("llamacpp", "rocm-stable").empty(),
              "builtin remains reserved after clearing the environment");
        lemon::RuntimeConfig::set_global(nullptr);
    }

    if (g_failures > 0) {
        std::cerr << "Total failures: " << g_failures << std::endl;
        return 1;
    }

    std::cout << "All GPU environment unit tests passed!" << std::endl;
    return 0;
}
