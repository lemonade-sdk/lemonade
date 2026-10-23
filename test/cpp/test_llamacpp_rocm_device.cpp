#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <lemon/backends/install_staging.h>
#include <lemon/rocm_arch_override.h>
#include <lemon/system_info.h>

namespace fs = std::filesystem;
using lemon::SystemInfo;

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition) {
        std::cout << "PASS: " << label << std::endl;
    } else {
        std::cout << "FAIL: " << label << std::endl;
        ++failures;
    }
}

}  // namespace

int main() {
    const fs::path asset_test_dir = fs::temp_directory_path() /
        ("llamacpp_rocm_asset_identity_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(asset_test_dir);
    {
        std::ofstream asset_file(asset_test_dir / "asset.txt");
        asset_file << "llama-b1332-ubuntu-rocm-gfx103X-x64.zip";
    }
    const std::string installed_asset = lemon::backends::read_installed_asset_identity(
        asset_test_dir);
    expect(!lemon::backends::installed_asset_identity_requires_refresh(
               installed_asset, "llama-b1332-ubuntu-rocm-gfx103X-x64.zip"),
           "the same ROCm nightly asset at the same version does not reinstall");
    expect(lemon::backends::installed_asset_identity_requires_refresh(
               installed_asset, "llama-b1332-ubuntu-rocm-gfx120X-x64.zip"),
           "a same-version ROCm nightly asset change triggers reinstall");
    fs::remove(asset_test_dir / "asset.txt");
    const std::string legacy_asset = lemon::backends::read_installed_asset_identity(
        asset_test_dir);
    expect(lemon::backends::installed_asset_identity_requires_refresh(
               legacy_asset, "llama-b1332-ubuntu-rocm-gfx120X-x64.zip"),
           "a legacy install without asset.txt triggers one reinstall");
    fs::remove_all(asset_test_dir);

#ifdef _WIN32
    expect(lemon::rocm_visibility_override_env_name("", "1", nullptr, nullptr) ==
               std::string("HIP_VISIBLE_DEVICES"),
           "Windows selects HIP_VISIBLE_DEVICES for an existing visibility override");
    expect(
        lemon::rocm_visibility_override_env_name("", nullptr, nullptr, nullptr) == nullptr,
           "Windows ignores the unsupported ROCR_VISIBLE_DEVICES override");
#else
    expect(lemon::rocm_visibility_override_env_name("", "1", nullptr, nullptr) ==
               std::string("ROCR_VISIBLE_DEVICES"),
           "ROCR_VISIBLE_DEVICES takes precedence on Linux");
#endif
    expect(lemon::rocm_visibility_override_env_name(nullptr, "", nullptr, nullptr) ==
               std::string("HIP_VISIBLE_DEVICES"),
           "HIP_VISIBLE_DEVICES is selected when it is the only override");
    expect(
        lemon::rocm_visibility_override_env_name(nullptr, nullptr, nullptr, nullptr) == nullptr,
           "no selector is returned when neither visibility variable exists");

    nlohmann::json hybrid = nlohmann::json::array({
        {{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"},
         {"integrated", true}, {"available", true}},
        {{"name", "AMD Instinct MI300X"}, {"family", "gfx942"},
         {"integrated", false}, {"available", true}}
    });
    expect(SystemInfo::select_rocm_arch(hybrid) == "gfx942",
           "default architecture selection prefers a discrete GPU on a hybrid host");

    nlohmann::json igpu_only = nlohmann::json::array({
        {{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"},
         {"integrated", true}, {"available", true}}
    });
    expect(SystemInfo::select_rocm_arch(igpu_only) == "gfx1151",
           "iGPU-only hosts resolve their integrated architecture");

    nlohmann::json dgpu_only = nlohmann::json::array({
        {{"name", "AMD Instinct MI300X"}, {"family", "gfx942"},
         {"integrated", false}, {"available", true}}
    });
    expect(SystemInfo::select_rocm_arch(dgpu_only) == "gfx942",
           "dGPU-only hosts resolve their discrete architecture");

    nlohmann::json unavailable_dgpu = nlohmann::json::array({
        {{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"},
         {"integrated", true}, {"available", true}},
        {{"name", "AMD Instinct MI300X"}, {"family", "gfx942"},
         {"integrated", false}, {"available", false}}
    });
    expect(SystemInfo::select_rocm_arch(unavailable_dgpu) == "gfx1151",
           "an unavailable dGPU falls back to the available iGPU");

    nlohmann::json dual_gpu = nlohmann::json::array({
        {{"name", "AMD Radeon RX 6800"}, {"family", "gfx1031"},
         {"integrated", false}, {"available", true}, {"rocm_index", 0}},
        {{"name", "AMD Radeon RX 9070"}, {"family", "gfx1201"},
         {"integrated", false}, {"available", true}, {"rocm_index", 1}}
    });
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm0") == "gfx1031",
           "ROCm0 selects the first ROCm-indexed GPU architecture");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm1") == "gfx1201",
           "ROCm1 selects the requested ROCm-indexed GPU architecture");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm") == "gfx1031",
           "bare ROCm keeps the default discrete GPU selection");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm0,CUDA1").empty(),
           "mixed-vendor device lists are rejected");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCmX").empty(),
           "malformed ROCm device indices are rejected");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm18446744073709551616").empty(),
           "overflowing ROCm device indices are rejected");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm0,").empty(),
           "empty trailing device selections are rejected");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "   ").empty(),
           "whitespace-only device selections are rejected");
    expect(SystemInfo::select_rocm_arch(dual_gpu, "ROCm0, ROCm1").empty(),
           "whitespace does not hide mixed-architecture selections");

    nlohmann::json kfd_ordered_gpu = nlohmann::json::array({
        {{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"},
         {"integrated", true}, {"available", true}, {"rocm_index", 1}},
        {{"name", "AMD Radeon RX 6800"}, {"family", "gfx1031"},
         {"integrated", false}, {"available", true}, {"rocm_index", 0}}
    });
    expect(SystemInfo::select_rocm_arch(kfd_ordered_gpu, "ROCm0") == "gfx1031",
           "ROCm0 follows the KFD runtime ordinal, not display order");
    expect(SystemInfo::select_rocm_arch(kfd_ordered_gpu, "ROCm1") == "gfx1151",
           "ROCm1 follows the KFD runtime ordinal for an iGPU");
    expect(SystemInfo::get_rocm_visible_devices(kfd_ordered_gpu, "ROCm0").empty(),
           "missing UUID metadata keeps KFD ordinal selection unpinned");

    nlohmann::json identical_gpu = nlohmann::json::array({
        {{"name", "AMD Radeon RX 9070"}, {"family", "gfx1201"},
         {"integrated", false}, {"available", true},
         {"uuid", "GPU-0000000000000001"}},
        {{"name", "AMD Radeon RX 9070"}, {"family", "gfx1201"},
         {"integrated", false}, {"available", true},
         {"uuid", "GPU-0000000000000002"}}
    });
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm0") ==
               "GPU-0000000000000001",
           "ROCm0 resolves to its stable UUID on identical GPUs");
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1") ==
               "GPU-0000000000000002",
           "ROCm1 resolves to the other stable UUID on identical GPUs");
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1,ROCm0") ==
               "GPU-0000000000000002,GPU-0000000000000001",
           "multi-GPU visibility preserves the requested device order");
    expect(SystemInfo::remap_rocm_device_selection("ROCm1,ROCm0") == "ROCm0,ROCm1",
           "UUID-filtered devices are remapped to llama.cpp child ordinals");

    identical_gpu[1].erase("uuid");
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1").empty(),
           "missing UUID metadata disables unsafe visibility filtering");
    identical_gpu[1]["uuid"] = "GPU-0000000000000001";
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1").empty(),
           "duplicate UUIDs disable ambiguous visibility filtering");
    identical_gpu[1]["uuid"] = "GPU-XX";
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1").empty(),
           "unsupported UUID values disable visibility filtering");
    identical_gpu[1]["uuid"] = 2;
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1").empty(),
           "non-string UUID values disable visibility filtering");
    identical_gpu[1]["uuid"] = "GPU-0000000000000002";
    identical_gpu[1]["available"] = "true";
    expect(SystemInfo::get_rocm_visible_devices(identical_gpu, "ROCm1").empty(),
           "non-boolean availability disables visibility filtering");

    nlohmann::json mixed_arch_gpu = nlohmann::json::array({
        {{"name", "AMD Radeon RX 6800"}, {"family", "gfx1031"},
         {"available", true}, {"rocm_index", 0}},
        {{"name", "AMD Radeon RX 9070"}, {"family", "gfx1201"},
         {"available", true}, {"rocm_index", 1}}
    });
    expect(SystemInfo::select_rocm_arch(mixed_arch_gpu, "ROCm0,ROCm1").empty(),
           "mixed ROCm asset families are rejected for a single backend build");
    expect(SystemInfo::select_rocm_arch(mixed_arch_gpu, "ROCm2").empty(),
           "an out-of-range ROCm index is rejected");

    nlohmann::json unavailable_gpu = nlohmann::json::array({
        {{"name", "AMD Radeon RX 6800"}, {"family", "gfx1031"},
         {"integrated", false}, {"available", true}, {"rocm_index", 0}},
        {{"name", "AMD Radeon RX 9070"}, {"family", "gfx1201"},
         {"integrated", false}, {"available", false}, {"rocm_index", 1}}
    });
    expect(SystemInfo::select_rocm_arch(unavailable_gpu, "ROCm1").empty(),
           "an unavailable explicitly selected GPU is rejected");

    if (failures != 0) {
        std::cout << failures << " assertion(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All llama.cpp ROCm device assertions passed" << std::endl;
    return 0;
}
