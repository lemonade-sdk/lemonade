#include <lemon/models/host_availability.h>

#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/model_manager.h>
#include <lemon/runtime_config.h>
#include <lemon/system_info.h>
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>

namespace lemon {
namespace models {

namespace {

// Helper function to parse physical memory string (e.g., "32.00 GB") to GB as double
// Returns 0.0 if parsing fails
double parse_physical_memory_gb(const std::string& memory_str) {
    if (memory_str.empty()) {
        return 0.0;
    }

    // Expected format: "XX.XX GB" or "XX GB"
    std::istringstream iss(memory_str);
    double value = 0.0;
    std::string unit;

    if (iss >> value >> unit) {
        // Convert to lowercase for comparison
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        if (unit == "gb") {
            return value;
        } else if (unit == "mb") {
            return value / 1024.0;
        } else if (unit == "tb") {
            return value * 1024.0;
        }
    }

    return 0.0;
}

double memory_of_device(json device, MemoryAllocBehavior mem_alloc_behavior) {
    // Get the maximum POSSIBLE accessible memory of the device in question,
    // taking into account the respective memory allocation behavior.

    double virtual_mem_gb = 0.0;
    double vram_gb = 0.0;

    if (device.contains("vram_gb")) {
        vram_gb = device["vram_gb"].get<double>();
    }
    if (device.contains("virtual_mem_gb"))
    {
        virtual_mem_gb = device["virtual_mem_gb"].get<double>();
    }

    switch (mem_alloc_behavior)
    {
    case MemoryAllocBehavior::Hardware:
        return vram_gb;

    case MemoryAllocBehavior::Virtual:
        return virtual_mem_gb;

    case MemoryAllocBehavior::Largest:
        return vram_gb > virtual_mem_gb ? vram_gb : virtual_mem_gb;

    case MemoryAllocBehavior::Unified:
        return virtual_mem_gb + vram_gb;

    default:
        return vram_gb;
    }
}

}  // namespace

double device_memory_pool_gb(const std::string& device_type, const nlohmann::json& device,
                             bool assume_dgpu_gtt) {
    // Behavior is chosen per device, not per device-type: AMD APUs are reported
    // under "amd_gpu" with "integrated": true, so an absent flag reads as integrated.
    MemoryAllocBehavior behavior = MemoryAllocBehavior::Hardware;
    if (device_type == "amd_gpu" && device.value("integrated", true)) {
        behavior = MemoryAllocBehavior::Largest;
    }
    if (assume_dgpu_gtt) {
        behavior = MemoryAllocBehavior::Unified;
    }
    return memory_of_device(device, behavior);
}

// Resolves everything the hardware filter needs from configuration and system
// info. Keeping the reads here is what lets filter_hardware() stay a pure
// function of its inputs instead of reaching into backend and system headers.
BackendAvailability probe_backend_availability(
    const std::map<std::string, ModelInfo>& models) {
    BackendAvailability availability;

    if (auto* cfg = lemon::RuntimeConfig::global()) {
        availability.filtering_disabled = cfg->disable_model_filtering();
        availability.assume_dgpu_gtt = cfg->enable_dgpu_gtt();
    }

    if (availability.filtering_disabled) {
        return availability;
    }

    if (availability.assume_dgpu_gtt) {
        LOG(INFO, "ModelManager") << "enable_dgpu_gtt has been set to true." << std::endl
                                  << "     Models are being filtered assuming GTT memory."
                                  << std::endl
                                  << "     Using GTT on a dGPU will have a significant "
                                     "performance impact."
                                  << std::endl;
    }

    json system_info = SystemInfoCache::get_system_info_with_cache();
    json hardware = system_info.contains("devices") ? system_info["devices"] : json::object();

    bool npu_available = hardware.contains("amd_npu") && hardware["amd_npu"].is_object() &&
                         hardware["amd_npu"].value("available", false);

    double largest_mem_pool_gb = 0.0;
    double curr_mem_pool_gb = 0.0;

    for (const auto& [dev_type, devices] : hardware.items()) {
        // Because we have mixed types this just makes every device_type an array.
        nlohmann::json dev_list = devices.is_array() ? devices : nlohmann::json{devices};

        for (const auto& dev : dev_list) {
            if (!dev.is_object())
                continue;

            curr_mem_pool_gb = device_memory_pool_gb(dev_type, dev, availability.assume_dgpu_gtt);
            largest_mem_pool_gb = largest_mem_pool_gb < curr_mem_pool_gb ? curr_mem_pool_gb
                                                                         : largest_mem_pool_gb;
        }
    }
    availability.largest_mem_pool_gb = largest_mem_pool_gb;

    double system_ram_gb = 0.0;
    if (system_info.contains("Physical Memory") && system_info["Physical Memory"].is_string()) {
        system_ram_gb = parse_physical_memory_gb(system_info["Physical Memory"].get<std::string>());
    }
    availability.system_ram_gb = system_ram_gb;

    if (system_info.contains("Processor") && system_info["Processor"].is_string()) {
        availability.processor = system_info["Processor"].get<std::string>();
    }
    if (system_info.contains("OS Version") && system_info["OS Version"].is_string()) {
        availability.os_version = system_info["OS Version"].get<std::string>();
    }

    static bool debug_printed = false;
    if (!debug_printed) {
        double max_model_size_gb = largest_mem_pool_gb > (system_ram_gb * 0.8)
                                       ? largest_mem_pool_gb
                                       : (system_ram_gb * 0.8);
        LOG(INFO, "ModelManager") << "Backend availability:" << std::endl;
        LOG(INFO, "ModelManager") << "  - NPU hardware: " << (npu_available ? "Yes" : "No")
                                  << std::endl;
        if (system_ram_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - System RAM: " << std::fixed << std::setprecision(1)
                                      << system_ram_gb << " GB (max model size: "
                                      << max_model_size_gb << " GB)" << std::endl;
        }
        if (largest_mem_pool_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - Largest memory pool: " << std::fixed
                                      << std::setprecision(1) << largest_mem_pool_gb << std::endl;
        }
        if (system_info.contains("devices") && system_info["devices"].contains("nvidia_gpu")) {
            const auto& nvidia_gpus = system_info["devices"]["nvidia_gpu"];
            if (nvidia_gpus.is_array()) {
                for (const auto& gpu : nvidia_gpus) {
                    if (gpu.value("available", false)) {
                        std::string gpu_name = gpu.value("name", "unknown");
                        std::string family = gpu.value("family", "");
                        std::string cc = gpu.value("compute_capability", "");
                        std::string suffix;
                        if (!cc.empty())
                            suffix = " (compute " + cc + ", " +
                                     (family.empty() ? "unsupported arch" : family) + ")";
                        else if (!family.empty())
                            suffix = " (" + family + ")";
                        else
                            suffix = " (arch unknown -- nvidia-smi may have failed)";
                        LOG(INFO, "ModelManager") << "  - NVIDIA GPU: " << gpu_name << suffix
                                                  << std::endl;
                    } else if (gpu.contains("error")) {
                        LOG(INFO, "ModelManager")
                            << "  - NVIDIA GPU: detection error: "
                            << gpu["error"].get<std::string>() << std::endl;
                    }
                }
                if (system_info["devices"].contains("nvidia_gpu_error")) {
                    LOG(INFO, "ModelManager")
                        << "  - NVIDIA GPU: detection error: "
                        << system_info["devices"]["nvidia_gpu_error"].get<std::string>()
                        << std::endl;
                }
            }
        }
        debug_printed = true;
    }

    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        if (availability.recipes.count(recipe)) {
            continue;
        }

        RecipeAvailability recipe_availability;
        recipe_availability.unsupported_reason = SystemInfo::check_recipe_supported(recipe);
        if (recipe_availability.unsupported_reason.empty()) {
            recipe_availability.guest_backend_missing_reason =
                SystemInfo::check_guest_backend_installed(recipe, system_info);
        }
        if (const auto* desc = backends::descriptor_for(recipe)) {
            recipe_availability.streams_model_from_storage = desc->streams_model_from_storage;
        }
        availability.recipes.emplace(recipe, std::move(recipe_availability));
    }

    return availability;
}

}  // namespace models
}  // namespace lemon
