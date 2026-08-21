// Direct coverage for the Linux llamacpp "system" backend resolver.
//
// These tests intentionally avoid lemond. They exercise the production PATH,
// HIP-plugin and SystemInfo selection code with controlled filesystem/config
// inputs, leaving subprocess/HTTP behavior to test_llamacpp_system_backend.py.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lemon/config_file.h>
#include <lemon/runtime_config.h>
#include <lemon/system_info.h>
#include <lemon/utils/path_utils.h>

#include "backends/llamacpp/llamacpp_system_utils.h"

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

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) {
            had_value_ = true;
            old_value_ = value;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    void set(const std::string& value) const {
        setenv(name_.c_str(), value.c_str(), 1);
    }

    void clear() const {
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::string old_value_;
    bool had_value_ = false;
};

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

class TempDir {
public:
    TempDir() {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
                ("lemonade_llamacpp_system_" + std::to_string(nonce));
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

class TestSystemInfo final : public lemon::SystemInfo {
public:
    lemon::CPUInfo get_cpu_device() override { return {}; }
    lemon::GPUInfo get_amd_igpu_device() override { return {}; }
    std::vector<lemon::GPUInfo> get_amd_dgpu_devices() override { return {}; }
    std::vector<lemon::GPUInfo> get_nvidia_gpu_devices() override { return {}; }
    lemon::NPUInfo get_npu_device() override { return {}; }
};

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
    out.close();
    check(static_cast<bool>(out), "writes test fixture " + path.string());
}

void make_executable(const fs::path& path) {
    std::error_code ec;
    fs::permissions(
        path,
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
        fs::perm_options::add,
        ec);
    check(!ec, "marks test llama-server executable");
}

std::string test_cpu_family() {
#if defined(__aarch64__)
    return "arm64";
#else
    return "x86_64";
#endif
}

json test_devices() {
    return {
        {"cpu",
         {
             {"name", "Test CPU"},
             {"available", true},
             {"family", test_cpu_family()},
         }},
    };
}

json build_recipes(TestSystemInfo& system_info,
                   const fs::path& path_dir,
                   const std::string& configured_backend,
                   std::optional<bool> prefer_system_override) {
    // build_recipes_info() evaluates every recipe/backend row, so give
    // RuntimeConfig the same complete canonical shape used by production.
    // base_defaults() intentionally excludes host/distro overrides.
    json config_json = lemon::ConfigFile::base_defaults();
    config_json["llamacpp"]["backend"] = configured_backend;
    if (prefer_system_override.has_value()) {
        config_json["llamacpp"]["prefer_system"] = *prefer_system_override;
    }
    lemon::RuntimeConfig config(config_json);
    ScopedRuntimeConfig config_scope(&config);
    ScopedEnvVar path_scope("PATH");
    path_scope.set(path_dir.string());
    return system_info.build_recipes_info(test_devices());
}

void check_system_state(const json& recipes,
                        const std::string& expected_state,
                        const std::string& message) {
    check(recipes.contains("llamacpp"), message + ": llamacpp recipe exists");
    if (!recipes.contains("llamacpp")) {
        return;
    }
    const auto& backends = recipes["llamacpp"]["backends"];
    check(backends.contains("system"), message + ": system backend exists");
    if (backends.contains("system")) {
        check(backends["system"].value("state", "") == expected_state,
              message + ": system state is " + expected_state);
    }
}

#endif  // __linux__

}  // namespace

int main() {
#ifndef __linux__
    std::cout << "llamacpp system backend resolver is Linux-only; skipped." << std::endl;
    return 0;
#else
    TempDir temp;
    const fs::path system_bin = temp.path() / "system-bin";
    const fs::path empty_bin = temp.path() / "empty-bin";
    const fs::path llama_server = system_bin / "llama-server";
    fs::create_directories(system_bin);
    fs::create_directories(empty_bin);
    write_file(llama_server, "#!/bin/sh\nprintf 'version: 9999 (mock)\\n'\n");
    make_executable(llama_server);

    ScopedEnvVar path_env("PATH");
    ScopedEnvVar hip_env("LEMONADE_GGML_HIP_PATH");

    // PATH discovery is a direct C++ decision: no lemond is needed to prove it.
    path_env.set(system_bin.string());
    check(!lemon::utils::find_executable_in_path("llama-server").empty(),
          "discovers executable llama-server from PATH");
    path_env.set(empty_bin.string());
    check(lemon::utils::find_executable_in_path("llama-server").empty(),
          "reports llama-server absent when PATH has no executable");

    // Validate the user-provided HIP override without requiring /sys/class/kfd
    // or relying on whichever libraries happen to be installed on the CI host.
    const fs::path valid_hip = temp.path() / "libggml-hip.so";
    const fs::path wrong_name = temp.path() / "not-the-hip-plugin.so";
    write_file(valid_hip, "stub");
    write_file(wrong_name, "stub");

    hip_env.set(valid_hip.string());
    check(lemon::backends::llamacpp::detail::ggml_hip_env_override_available(),
          "accepts a regular libggml-hip.so environment override");

    hip_env.set((temp.path() / "missing" / "libggml-hip.so").string());
    check(!lemon::backends::llamacpp::detail::ggml_hip_env_override_available(),
          "rejects a nonexistent HIP environment override");

    hip_env.set(temp.path().string());
    check(!lemon::backends::llamacpp::detail::ggml_hip_env_override_available(),
          "rejects a directory as the HIP environment override");

    hip_env.set(wrong_name.string());
    check(!lemon::backends::llamacpp::detail::ggml_hip_env_override_available(),
          "rejects an unrelated regular file as the HIP environment override");

    // The PATH layout resolver also stays deterministic: test both layouts
    // supported by production without looking at the host's system libraries.
    const fs::path layout_root = temp.path() / "layout";
    const fs::path layout_bin = layout_root / "bin";
    const fs::path layout_server = layout_bin / "llama-server";
    const fs::path adjacent_hip = layout_bin / "libggml-hip.so";
    const fs::path sibling_hip = layout_root / "lib" / "libggml-hip.so";
    write_file(layout_server, "#!/bin/sh\nexit 0\n");
    make_executable(layout_server);
    write_file(adjacent_hip, "stub");
    check(lemon::backends::llamacpp::detail::ggml_hip_plugin_near_llama_server(
              layout_bin.string()),
          "finds libggml-hip.so next to PATH llama-server");

    fs::remove(adjacent_hip);
    write_file(sibling_hip, "stub");
    check(lemon::backends::llamacpp::detail::ggml_hip_plugin_near_llama_server(
              layout_bin.string()),
          "finds libggml-hip.so in sibling lib directory");

    fs::remove(sibling_hip);
    check(!lemon::backends::llamacpp::detail::ggml_hip_plugin_near_llama_server(
              layout_bin.string()),
          "does not invent a HIP plugin for a PATH llama-server");

    // Keep the real SystemInfo resolver independent of the CI machine's AMD
    // state. If KFD is present, this valid override satisfies the same check
    // that production performs for the system backend.
    hip_env.set(valid_hip.string());
    TestSystemInfo system_info;

    // No override here: exercise the canonical prefer_system default shipped
    // through ConfigFile::base_defaults().
    const json default_preference = build_recipes(
        system_info, system_bin, "auto", std::nullopt);
    check_system_state(default_preference, "installed",
                       "available system backend with default config");
    check(default_preference["llamacpp"].value("default_backend", "") == "system",
          "default config prefers an available system backend");

    const json not_preferred = build_recipes(
        system_info, system_bin, "auto", /*prefer_system=*/false);
    check_system_state(not_preferred, "installed",
                       "available system backend with prefer_system=false");
    check(not_preferred["llamacpp"].value("default_backend", "") != "system",
          "prefer_system=false keeps an available system backend out of default selection");

    const json preferred = build_recipes(
        system_info, system_bin, "auto", /*prefer_system=*/true);
    check_system_state(preferred, "installed",
                       "available system backend with prefer_system=true");
    check(preferred["llamacpp"].value("default_backend", "") == "system",
          "prefer_system=true selects the available system backend");

    const json explicit_system = build_recipes(
        system_info, system_bin, "system", /*prefer_system=*/false);
    check(explicit_system["llamacpp"].value("default_backend", "") == "system",
          "explicit llamacpp.backend=system overrides prefer_system=false");

    const json unavailable = build_recipes(
        system_info, empty_bin, "auto", /*prefer_system=*/true);
    check_system_state(unavailable, "unsupported",
                       "missing system backend with prefer_system=true");
    check(unavailable["llamacpp"].value("default_backend", "") != "system",
          "prefer_system=true falls back when system llama-server is unavailable");
    if (unavailable.contains("llamacpp") &&
        unavailable["llamacpp"]["backends"].contains("system")) {
        check(unavailable["llamacpp"]["backends"]["system"]
                      .value("message", "")
                      .find("llama-server not found in PATH") != std::string::npos,
              "missing system backend reports PATH discovery failure");
    }

    const json unavailable_not_preferred = build_recipes(
        system_info, empty_bin, "auto", /*prefer_system=*/false);
    check_system_state(unavailable_not_preferred, "unsupported",
                       "missing system backend with prefer_system=false");
    check(unavailable_not_preferred["llamacpp"].value("default_backend", "") != "system",
          "prefer_system=false also falls back when system llama-server is unavailable");

    if (failures != 0) {
        std::cerr << "Total failures: " << failures << std::endl;
        return 1;
    }

    std::cout << "All llamacpp system backend resolver tests passed!" << std::endl;
    return 0;
#endif
}
