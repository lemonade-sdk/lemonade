// Unit tests for lemon::backends::BackendUtils::validate_device_backend_match()
// and lemon::backends::BackendUtils::apply_cuda_env_vars().
//
// These tests exercise environment variable override precedence and device
// selection validation deterministically without requiring physical GPU hardware.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <lemon/backends/backend_utils.h>
#include <lemon/runtime_config.h>
#include <lemon/recipe_options.h>

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

    // Test 2b: explicit CUDA selection uses the physical GPU UUID, not CUDA
    // runtime ordinals (which can differ from nvidia-smi on 4090 + 5090 hosts).
    {
        const std::vector<std::pair<int, std::string>> gpus = {
            {0, "GPU-test-4090"}, {1, "GPU-test-5090"}
        };
        check(BackendUtils::cuda_visible_device_for_selection("CUDA1", gpus) == "GPU-test-5090",
              "CUDA1 resolves to the matching NVIDIA GPU UUID");
        check(BackendUtils::cuda_visible_device_for_selection("cuda0", gpus) == "GPU-test-4090",
              "CUDA0 resolves to the other NVIDIA GPU UUID");
        bool rejected = false;
        try { (void)BackendUtils::cuda_visible_device_for_selection("cuda2", gpus); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "nonexistent CUDA device is rejected");
        rejected = false;
        try { (void)BackendUtils::cuda_visible_device_for_selection("cuda0,cuda1", gpus); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "multi-GPU string is rejected for single-device backends");
        rejected = false;
        try { (void)BackendUtils::cuda_visible_device_for_selection("cuda1", {{0, "GPU-0"}, {1, ""}}); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "missing UUID is rejected instead of selecting an ambiguous CUDA ordinal");
        check(BackendUtils::cuda_visible_device_for_selection("CUDA0", {{0, "GPU-one"}}) == "0",
              "single NVIDIA GPU gets an explicit numeric visibility mask");
        check(BackendUtils::cuda_visible_device_for_selection("CUDA0", {{0, ""}}) == "0",
              "sole GPU with no UUID gets an explicit numeric mask");
        bool valid_arch = true;
        try {
            BackendUtils::validate_cuda_device_arch_for_selection("CUDA1", "sm_120",
                {{0, "sm_89"}, {1, "sm_120"}});
        } catch (...) { valid_arch = false; }
        check(valid_arch, "CUDA device matching sd.cpp asset architecture is accepted");
        rejected = false;
        try {
            BackendUtils::validate_cuda_device_arch_for_selection("CUDA0", "sm_120",
                {{0, "sm_89"}, {1, "sm_120"}});
        } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "CUDA device incompatible with installed sd.cpp asset is rejected");
        check(BackendUtils::rocm_visible_device_for_selection("ROCm1", 2) == "1",
              "ROCm1 maps to HIP_VISIBLE_DEVICES=1");
        rejected = false;
        try { (void)BackendUtils::rocm_visible_device_for_selection("ROCm2", 2); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "nonexistent ROCm device is rejected");
    }

    // Test 2c: explicit auto device selection masks any inherited model value.
    {
        const lemon::RecipeOptions thenoise_auto("thenoise", {{"thenoise_device", "auto"}});
        const lemon::RecipeOptions thenoise_model("thenoise", {{"thenoise_device", "CUDA0"}});
        check(thenoise_auto.inherit(thenoise_model).get_option("thenoise_device") == "auto",
              "TheNoise explicit auto clears inherited device");

        const lemon::RecipeOptions sd_auto("sd-cpp", {{"sd-cpp_device", "auto"}});
        const lemon::RecipeOptions sd_model("sd-cpp", {{"sd-cpp_device", "CUDA0"}});
        check(sd_auto.inherit(sd_model).get_option("sd-cpp_device") == "auto",
              "sd.cpp explicit auto clears inherited device");
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

    // Test 5: a *_bin directory (the documented form) resolves to the backend
    // binary inside it, since callers exec the returned path. A file path is
    // returned as-is, and a directory without the binary is left unchanged.
    {
        namespace fs = std::filesystem;
        const auto* spec = lemon::backends::try_get_spec_for_recipe("llamacpp");
        check(spec != nullptr && !spec->binary.empty(), "llamacpp spec has a binary name");
        if (spec != nullptr && !spec->binary.empty()) {
            clear_env_var("LEMONADE_LLAMACPP_VULKAN_BIN");
#ifdef _WIN32
            const int pid = _getpid();
#else
            const int pid = getpid();
#endif
            const fs::path dir = fs::temp_directory_path() / ("lemonade_bin_dir_test_" + std::to_string(pid));
            const fs::path empty_dir = dir / "empty";
            fs::remove_all(dir);
            fs::create_directories(empty_dir);
            const fs::path binary = dir / spec->binary;
            std::ofstream(binary.string()) << "#!/bin/sh\n";

            lemon::RuntimeConfig dir_config(lemon::json{{"llamacpp", {{"vulkan_bin", dir.string()}}}});
            lemon::RuntimeConfig::set_global(&dir_config);
            check(BackendUtils::find_external_backend_binary("llamacpp", "vulkan") == binary.string(),
                  "directory *_bin resolves to the backend binary inside it");

            lemon::RuntimeConfig file_config(lemon::json{{"llamacpp", {{"vulkan_bin", binary.string()}}}});
            lemon::RuntimeConfig::set_global(&file_config);
            check(BackendUtils::find_external_backend_binary("llamacpp", "vulkan") == binary.string(),
                  "file *_bin is returned unchanged");

            lemon::RuntimeConfig empty_config(lemon::json{{"llamacpp", {{"vulkan_bin", empty_dir.string()}}}});
            lemon::RuntimeConfig::set_global(&empty_config);
            check(BackendUtils::find_external_backend_binary("llamacpp", "vulkan") == empty_dir.string(),
                  "directory *_bin without the binary is returned unchanged");

            lemon::RuntimeConfig::set_global(nullptr);
            fs::remove_all(dir);
        }
    }

    if (g_failures > 0) {
        std::cerr << "Total failures: " << g_failures << std::endl;
        return 1;
    }

    std::cout << "All GPU environment unit tests passed!" << std::endl;
    return 0;
}
