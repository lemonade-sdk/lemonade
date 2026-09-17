#include "lemon_cli/bench_sysinfo.h"
#include <algorithm>
#include <iostream>
#include <string>

namespace lemon_cli {

using json = nlohmann::json;

static double parse_gb_string(const std::string& s) {
    std::string num_str = s;
    size_t space_pos = num_str.find(' ');
    if (space_pos != std::string::npos) {
        num_str = num_str.substr(0, space_pos);
    }
    try {
        return std::stod(num_str);
    } catch (...) {
        return -1.0;
    }
}

// ============================================================
// Public API
// ============================================================

json build_hardware_profile(const json& sys_info,
                            const std::vector<std::string>& recipes,
                            const std::vector<std::string>& backends,
                            const std::string& lemonade_version) {
    json profile;

    if (!lemonade_version.empty()) {
        profile["lemonade_version"] = lemonade_version;
    }

    try {
        std::string os_version = sys_info.value("OS Version", "unknown");
        std::string os;
        if (os_version.find("Windows") != std::string::npos) os = "windows";
        else if (os_version.find("macOS") != std::string::npos || os_version.find("Darwin") != std::string::npos) os = "macos";
        else if (os_version.find("Linux") != std::string::npos) os = "linux";
        else os = os_version;
        profile["os"] = os;

        if (sys_info.contains("devices") && sys_info["devices"].contains("cpu")) {
            profile["cpu"] = sys_info["devices"]["cpu"].value("name", "unknown");
        } else {
            profile["cpu"] = sys_info.value("Processor", "unknown");
        }

        if (sys_info.contains("Physical Memory") && sys_info["Physical Memory"].is_string()) {
            double ram_gb = parse_gb_string(sys_info["Physical Memory"].get<std::string>());
            if (ram_gb > 0) {
                profile["ram_gb"] = ram_gb;
            }
        }

        json gpu_array = json::array();
        if (sys_info.contains("devices")) {
            const auto& devices = sys_info["devices"];

            if (devices.contains("amd_gpu") && devices["amd_gpu"].is_array()) {
                for (const auto& gpu : devices["amd_gpu"]) {
                    if (gpu.value("available", false)) {
                        json gpu_entry;
                        gpu_entry["name"] = gpu.value("name", "unknown");
                        gpu_entry["type"] = "amd";
                        gpu_entry["integrated"] = gpu.value("integrated", false);
                        if (gpu.contains("vram_gb") && gpu["vram_gb"].is_number()) {
                            gpu_entry["vram_gb"] = gpu["vram_gb"].get<double>();
                        }
                        gpu_array.push_back(gpu_entry);
                    }
                }
            }

            if (devices.contains("nvidia_gpu") && devices["nvidia_gpu"].is_array()) {
                for (const auto& gpu : devices["nvidia_gpu"]) {
                    if (gpu.value("available", false)) {
                        json gpu_entry;
                        gpu_entry["name"] = gpu.value("name", "unknown");
                        gpu_entry["type"] = "nvidia";
                        if (gpu.contains("vram_gb") && gpu["vram_gb"].is_number()) {
                            gpu_entry["vram_gb"] = gpu["vram_gb"].get<double>();
                        }
                        gpu_array.push_back(gpu_entry);
                    }
                }
            }

            if (devices.contains("metal") && devices["metal"].is_object()) {
                const auto& metal = devices["metal"];
                if (metal.value("available", false)) {
                    json gpu_entry;
                    gpu_entry["name"] = metal.value("name", "unknown");
                    gpu_entry["type"] = "metal";
                    if (metal.contains("vram_gb") && metal["vram_gb"].is_number()) {
                        gpu_entry["vram_gb"] = metal["vram_gb"].get<double>();
                    }
                    gpu_array.push_back(gpu_entry);
                }
            }
        }
        profile["gpu"] = gpu_array;

        json backend_versions = json::object();
        if (sys_info.contains("recipes") && sys_info["recipes"].is_object()) {
            for (const auto& [recipe_name, recipe_data] : sys_info["recipes"].items()) {
                if (!recipes.empty()) {
                    bool found = std::find(recipes.begin(), recipes.end(), recipe_name) != recipes.end();
                    if (!found) continue;
                }

                if (!recipe_data.contains("backends") || !recipe_data["backends"].is_object()) {
                    continue;
                }

                for (const auto& [backend_name, backend_data] : recipe_data["backends"].items()) {
                    if (!backends.empty()) {
                        bool found = std::find(backends.begin(), backends.end(), backend_name) != backends.end();
                        if (!found) continue;
                    }

                    std::string state = backend_data.value("state", "unknown");
                    if (state != "installed" && state != "update_required" && state != "update_available") {
                        continue;
                    }

                    std::string version = backend_data.value("version", "");
                    if (!version.empty()) {
                        backend_versions[recipe_name + "/" + backend_name] = version;
                    }
                }
            }
        }
        if (!backend_versions.empty()) {
            profile["backends"] = backend_versions;
        }

    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to build hardware profile: " << e.what() << std::endl;
    }

    return profile;
}

} // namespace lemon_cli
