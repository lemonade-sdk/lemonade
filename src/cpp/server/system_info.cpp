#include "lemon/system_info.h"
#include "lemon/version.h"
#include "lemon/utils/path_utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include "utils/wmi_helper.h"
#pragma comment(lib, "wbemuuid.lib")
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace lemon {

namespace fs = std::filesystem;
using namespace lemon::utils;

// AMD discrete GPU keywords
const std::vector<std::string> AMD_DISCRETE_GPU_KEYWORDS = {
    "rx ", "xt", "pro w", "pro v", "radeon pro", "firepro", "fury"
};

// NVIDIA discrete GPU keywords
const std::vector<std::string> NVIDIA_DISCRETE_GPU_KEYWORDS = {
    "geforce", "rtx", "gtx", "quadro", "tesla", "titan",
    "a100", "a40", "a30", "a10", "a6000", "a5000", "a4000", "a2000"
};

// ROCm architecture mapping - maps specific gfx architectures to their family
const std::map<std::string, std::string> ROCM_ARCH_MAPPING = {
    // RDNA4 family (gfx120X)
    {"gfx1200", "gfx120X"},
    {"gfx1201", "gfx120X"},

    // RDNA3 family (gfx110X)
    {"gfx1100", "gfx110X"},
    {"gfx1101", "gfx110X"},
    {"gfx1102", "gfx110X"},
    {"gfx1103", "gfx110X"},
};

// ============================================================================
// Recipe/Backend definition table - single source of truth for support matrix
// ============================================================================

// Device constraints: device_type -> set of allowed families (empty = all families)
using DeviceConstraints = std::map<std::string, std::set<std::string>>;

struct RecipeBackendDef {
    std::string recipe;
    std::string backend;
    std::set<std::string> supported_os;
    DeviceConstraints devices;
};

// Recipe definitions table - single source of truth for all recipe/backend support
// Format: {recipe, backend, {supported_os}, {{device_type, {allowed_families}}}}
//
// IMPORTANT: Backend order matters! For recipes with multiple backends (e.g., llamacpp),
// the order in this table defines the preference order. First listed = most preferred.
// Example: metal is listed before vulkan on macOS, vulkan before cpu elsewhere.
//
// Empty family set {} means "all families of that device type"
static const std::vector<RecipeBackendDef> RECIPE_DEFS = {
    // llamacpp with multiple backends (order = preference: metal > vulkan > rocm > cpu)
    {"llamacpp", "metal", {"macos"},
    {
        {"metal", {}},
    }},
    {"llamacpp", "vulkan", {"windows", "linux"}, {
        {"cpu", {"x86_64"}},
        {"amd_igpu", {}},      // all iGPU families
        {"amd_dgpu", {}},      // all dGPU families
    }},
    {"llamacpp", "rocm", {"windows", "linux"}, {
        {"amd_igpu", {"gfx1150", "gfx1151"}},                      // STX Point/Halo iGPUs
        {"amd_dgpu", {"gfx110X", "gfx120X"}},                      // RDNA3/RDNA4 dGPUs
    }},
    {"llamacpp", "cpu", {"windows", "linux"}, {
        {"cpu", {"x86_64"}},
    }},

    // whisper.cpp - Windows x86_64 only
    {"whispercpp", "default", {"windows"}, {
        {"cpu", {"x86_64"}},
    }},

    // kokoro - Windows/Linux x86_64
    {"kokoro", "cpu", {"windows", "linux"}, {
        {"cpu", {"x86_64"}},
    }},

    // stable-diffusion.cpp - Windows/Linux x86_64
    {"sd-cpp", "default", {"windows", "linux"}, {
        {"cpu", {"x86_64"}},
    }},

    // FLM - Windows NPU (XDNA2)
    {"flm", "default", {"windows"}, {
        {"npu", {"XDNA2"}},
    }},

    // OGA recipes - Windows NPU (XDNA2)
    {"oga-npu", "default", {"windows"}, {
        {"npu", {"XDNA2"}},
    }},
    {"oga-hybrid", "default", {"windows"}, {
        {"npu", {"XDNA2"}},
    }},
    {"oga-cpu", "default", {"windows"}, {
        {"npu", {"XDNA2"}},
    }},
};

// ============================================================================
// Device family to human-readable name mapping
// ============================================================================

// Maps device family codes to human-readable descriptions
// Format: {family_code, human_readable_name}
static const std::map<std::string, std::string> DEVICE_FAMILY_NAMES = {
    // CPU architectures
    {"x86_64", "x86-64 processors"},
    {"arm64", "ARM64 processors"},

    // AMD iGPU/dGPU architectures (ROCm)
    {"gfx1150", "Radeon 880M/890M (Strix Point)"},
    {"gfx1151", "Radeon 8050S/8060S (Strix Halo)"},
    {"gfx110X", "Radeon RX 7000 series (RDNA3)"},
    {"gfx120X", "Radeon RX 9000 series (RDNA4)"},

    // NPU architectures
    {"XDNA2", "AMD Ryzen AI 300/400 series or Z2 Extreme NPU"},
};

// Maps device types to human-readable names (for error messages)
static const std::map<std::string, std::string> DEVICE_TYPE_NAMES = {
    {"cpu", "CPU"},
    {"amd_igpu", "AMD iGPU"},
    {"amd_dgpu", "AMD dGPU"},
    {"npu", "NPU"},
    {"nvidia_dgpu", "NVIDIA GPU"},
    {"metal", "MacOS Metal GPU"}
};

// Get human-readable name for a device family (e.g., "gfx1150" -> "Radeon 880M/890M")
static std::string get_family_name(const std::string& family) {
    auto it = DEVICE_FAMILY_NAMES.find(family);
    return it != DEVICE_FAMILY_NAMES.end() ? it->second : family;
}

// Get human-readable name for a device type (e.g., "amd_igpu" -> "AMD iGPU")
static std::string get_device_type_name(const std::string& device_type) {
    auto it = DEVICE_TYPE_NAMES.find(device_type);
    return it != DEVICE_TYPE_NAMES.end() ? it->second : device_type;
}

// Generate a human-readable error message for unsupported backend
// Uses RECIPE_DEFS and DEVICE_FAMILY_NAMES to build a descriptive message
std::string SystemInfo::get_unsupported_backend_error(const std::string& recipe, const std::string& backend) {
    std::string error;

    // Find the recipe/backend in RECIPE_DEFS
    for (const auto& def : RECIPE_DEFS) {
        if (def.recipe == recipe && def.backend == backend) {
            // Collect all required family names
            std::vector<std::string> family_names;
            for (const auto& [device_type, families] : def.devices) {
                for (const auto& f : families) {
                    family_names.push_back(get_family_name(f));
                }
            }

            // Build error message
            error = "No compatible device detected for " + recipe;
            if (backend != "default") {
                error += " (" + backend + " backend)";
            }
            if (!family_names.empty()) {
                error += ". Requires: ";
                for (size_t i = 0; i < family_names.size(); i++) {
                    if (i > 0) error += ", ";
                    error += family_names[i];
                }
            }
            error += ".";
            break;
        }
    }

    if (error.empty()) {
        error = "Unsupported recipe/backend combination: " + recipe + "/" + backend;
    }

    return error;
}

// Detected device with its family
struct DetectedDevice {
    std::string type;      // "cpu", "amd_igpu", "amd_dgpu", "npu"
    std::string name;      // Full device name
    std::string family;    // "x86_64", "gfx1150", "XDNA2", etc.
    bool present;
};

// Get current OS identifier
static std::string get_current_os() {
    #ifdef _WIN32
    return "windows";
    #elif defined(__APPLE__)
    return "macos";
    #else
    return "linux";
    #endif
}

// Forward declarations for helper functions used in device detection
std::string identify_rocm_arch_from_name(const std::string& device_name);
std::string identify_npu_arch(const std::string& processor_name);

// Get device family from device name
// cpu_name is required for NPU detection (pass empty string for other device types)
static std::string get_device_family(const std::string& device_type, const std::string& device_name,
                                      const std::string& cpu_name) {
    if (device_type == "cpu") {
        #if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
        return "x86_64";
        #elif defined(__aarch64__) || defined(_M_ARM64)
        return "arm64";
        #else
        return "unknown";
        #endif
    }

    if (device_type == "amd_igpu" || device_type == "amd_dgpu") {
        return identify_rocm_arch_from_name(device_name);
    }

    if (device_type == "npu") {
        // Use the processor name to identify NPU architecture
        // The device_name for NPU is typically "AMD NPU" which isn't useful,
        // so we need the processor name (always available from cached devices)
        return identify_npu_arch(cpu_name);
    }

    if (device_type == "metal") {
        return "metal";
    }

    return "";
}

// Check if device matches constraints (empty constraint set = all families allowed)
static bool device_matches_constraint(const std::string& device_family,
                                       const std::set<std::string>& allowed_families) {
    if (allowed_families.empty()) {
        return true;  // Empty = all families allowed
    }
    return allowed_families.count(device_family) > 0;
}

// Generic installation check
static bool is_recipe_installed(const std::string& recipe, const std::string& backend) {
    if (recipe == "llamacpp") {
        return SystemInfo::is_llamacpp_installed(backend);
    }
    if (recipe == "whispercpp") {
        return SystemInfo::is_whispercpp_installed();
    }
    if (recipe == "kokoro") {
        return SystemInfo::is_kokoro_installed(backend);
    }
    if (recipe == "sd-cpp") {
        return SystemInfo::is_sdcpp_installed();
    }
    if (recipe == "flm") {
        // Check if FLM is installed
        #ifdef _WIN32
        for (const auto& path : {"C:\\Program Files\\AMD\\FLM\\flm.exe",
                                  "C:\\Program Files (x86)\\AMD\\FLM\\flm.exe"}) {
            if (fs::exists(path)) {
                return true;
            }
        }
        // Check PATH for non-standard installations
        FILE* pipe = _popen("where flm 2>NUL", "r");
        if (pipe) {
            char buffer[256];
            bool found = (fgets(buffer, sizeof(buffer), pipe) != nullptr);
            _pclose(pipe);
            return found;
        }
        #endif
        return false;
    }
    if (recipe.find("oga") == 0) {  // oga-npu, oga-hybrid, oga-cpu
        return SystemInfo::is_ryzenai_serve_available();
    }
    return false;
}

// Generic version check
static std::string get_recipe_version(const std::string& recipe, const std::string& backend) {
    if (recipe == "llamacpp") {
        return SystemInfo::get_llamacpp_version(backend);
    }
    if (recipe == "whispercpp") {
        return SystemInfo::get_whispercpp_version();
    }
    if (recipe == "kokoro") {
        return SystemInfo::get_kokoro_version(backend);
    }
    if (recipe == "sd-cpp") {
        return SystemInfo::get_sdcpp_version();
    }
    if (recipe == "flm") {
        return SystemInfo::get_flm_version();
    }
    if (recipe.find("oga") == 0) {  // oga-npu, oga-hybrid, oga-cpu
        return SystemInfo::get_oga_version();
    }
    return "";
}

// ============================================================================
// SystemInfo base class implementation
// ============================================================================

json SystemInfo::get_system_info_dict() {
    json info;
    info["OS Version"] = get_os_version();
    return info;
}

json SystemInfo::get_device_dict() {
    json devices;

    // NOTE: This function collects hardware info only (no inference engines).
    // Inference engines are detected separately in get_system_info_with_cache()
    // because they should always be fresh (not cached).

    // Get CPU info - with fault tolerance
    try {
        auto cpu = get_cpu_device();
        devices["cpu"] = {
            {"name", cpu.name},
            {"cores", cpu.cores},
            {"threads", cpu.threads},
            {"available", cpu.available}
        };
        if (!cpu.error.empty()) {
            devices["cpu"]["error"] = cpu.error;
        }
    } catch (const std::exception& e) {
        devices["cpu"] = {
            {"name", "Unknown"},
            {"cores", 0},
            {"threads", 0},
            {"available", true},  // Assume available - trust the user
            {"error", std::string("Detection exception: ") + e.what()}
        };
    }

    // Get AMD iGPU info - with fault tolerance
    try {
        auto amd_igpu = get_amd_igpu_device();
        devices["amd_igpu"] = {
            {"name", amd_igpu.name},
            {"vram_gb", amd_igpu.vram_gb},
            {"virtual_mem_gb", amd_igpu.virtual_gb},
            {"available", amd_igpu.available}
        };
        if (!amd_igpu.error.empty()) {
            devices["amd_igpu"]["error"] = amd_igpu.error;
        }
    } catch (const std::exception& e) {
        devices["amd_igpu"] = {
            {"name", "Unknown"},
            {"available", true},  // Assume available - trust the user
            {"error", std::string("Detection exception: ") + e.what()}
        };
    }

    // Get AMD dGPU info - with fault tolerance
    try {
        auto amd_dgpus = get_amd_dgpu_devices();
        devices["amd_dgpu"] = json::array();
        for (const auto& gpu : amd_dgpus) {
            json gpu_json = {
                {"name", gpu.name},
                {"available", gpu.available}
            };
            if (gpu.vram_gb > 0) {
                gpu_json["vram_gb"] = gpu.vram_gb;
            }
            if (gpu.virtual_gb > 0) {
                gpu_json["virtual_mem_gb"] = gpu.virtual_gb;
            }
            if (!gpu.driver_version.empty()) {
                gpu_json["driver_version"] = gpu.driver_version;
            }
            if (!gpu.error.empty()) {
                gpu_json["error"] = gpu.error;
            }
            devices["amd_dgpu"].push_back(gpu_json);
        }
    } catch (const std::exception& e) {
        devices["amd_dgpu"] = json::array();
        devices["amd_dgpu_error"] = std::string("Detection exception: ") + e.what();
    }

    // Get NVIDIA dGPU info - with fault tolerance
    try {
        auto nvidia_dgpus = get_nvidia_dgpu_devices();
        devices["nvidia_dgpu"] = json::array();
        for (const auto& gpu : nvidia_dgpus) {
            json gpu_json = {
                {"name", gpu.name},
                {"available", gpu.available}
            };
            if (gpu.vram_gb > 0) {
                gpu_json["vram_gb"] = gpu.vram_gb;
            }
            if (!gpu.driver_version.empty()) {
                gpu_json["driver_version"] = gpu.driver_version;
            }
            if (!gpu.error.empty()) {
                gpu_json["error"] = gpu.error;
            }
            devices["nvidia_dgpu"].push_back(gpu_json);
        }
    } catch (const std::exception& e) {
        devices["nvidia_dgpu"] = json::array();
        devices["nvidia_dgpu_error"] = std::string("Detection exception: ") + e.what();
    }

    // Get NPU info - with fault tolerance
    try {
        auto npu = get_npu_device();
        devices["npu"] = {
            {"name", npu.name},
            {"available", npu.available}
        };
        if (!npu.power_mode.empty()) {
            devices["npu"]["power_mode"] = npu.power_mode;
        }
        if (!npu.error.empty()) {
            devices["npu"]["error"] = npu.error;
        }
    } catch (const std::exception& e) {
        #ifdef _WIN32
        // On Windows, assume NPU may be available - trust the user
        devices["npu"] = {
            {"name", "Unknown"},
            {"available", true},
            {"error", std::string("Detection exception: ") + e.what()}
        };
        #else
        devices["npu"] = {
            {"name", "Unknown"},
            {"available", false},
            {"error", std::string("Detection exception: ") + e.what()}
        };
        #endif
    }

    #ifdef __APPLE__
    // Get Metal GPU info (macOS only) - with fault tolerance
    try {
        auto* mac_info = dynamic_cast<MacOSSystemInfo*>(this);

        if (mac_info) {
            auto metal_gpus = dynamic_cast<MacOSSystemInfo*>(this)->detect_metal_gpus();
            if (!metal_gpus.empty() && metal_gpus[0].available) {
                // Use first available Metal GPU (similar to how single devices are handled)
                const auto& gpu = metal_gpus[0];
                devices["metal"] = {
                    {"name", gpu.name},
                    {"available", gpu.available}
                };
                if (gpu.vram_gb > 0) {
                    devices["metal"]["vram_gb"] = gpu.vram_gb;
                }
                if (!gpu.driver_version.empty()) {
                    devices["metal"]["driver_version"] = gpu.driver_version;
                }
                if (!gpu.error.empty()) {
                    devices["metal"]["error"] = gpu.error;
                }
            } else {
                devices["metal"] = {
                    {"name", "Unknown"},
                    {"available", false},
                    {"error", "No Metal-compatible GPU found"}
                };
            }
        }
        else {
            devices["metal"] = {
                {"name", "Unknown"},
                {"available", false},
                {"error", std::string("Detection exception: ")}
            };
        }
    } catch (const std::exception& e) {
        devices["metal"] = {
            {"name", "Unknown"},
            {"available", false},
            {"error", std::string("Detection exception: ") + e.what()}
        };
    }
    #endif
    
    return devices;
}

std::string SystemInfo::get_os_version() {
    // Platform-specific implementation would go here
    // For now, return a basic string
    #ifdef _WIN32
    return "Windows";
    #elif __linux__
    return "Linux";
    #elif __APPLE__
    return "macOS";
    #else
    return "Unknown";
    #endif
}

json SystemInfo::build_recipes_info(const json& devices) {
    json recipes;

    // Get current OS
    std::string current_os = get_current_os();

    std::vector<DetectedDevice> detected_devices;

    // Get CPU name for NPU family detection
    std::string cpu_name;
    if (devices.contains("cpu") && devices["cpu"].is_object()) {
        cpu_name = devices["cpu"].value("name", "");
    }

    // Build detected_devices from devices JSON
    // CPU is always present
    if (devices.contains("cpu")) {
        const auto& cpu = devices["cpu"];
        std::string name = cpu.value("name", "CPU");
        detected_devices.push_back({"cpu", name, get_device_family("cpu", "", ""), true});
    } else {
        detected_devices.push_back({"cpu", "CPU", get_device_family("cpu", "", ""), true});
    }

    // AMD iGPU
    if (devices.contains("amd_igpu") && devices["amd_igpu"].is_object()) {
        const auto& igpu = devices["amd_igpu"];
        if (igpu.value("available", false)) {
            std::string name = igpu.value("name", "");
            if (!name.empty()) {
                detected_devices.push_back({
                    "amd_igpu",
                    name,
                    get_device_family("amd_igpu", name, ""),
                    true
                });
            }
        }
    }

    // AMD dGPUs
    if (devices.contains("amd_dgpu") && devices["amd_dgpu"].is_array()) {
        for (const auto& gpu : devices["amd_dgpu"]) {
            if (gpu.value("available", false)) {
                std::string name = gpu.value("name", "");
                if (!name.empty()) {
                    detected_devices.push_back({
                        "amd_dgpu",
                        name,
                        get_device_family("amd_dgpu", name, ""),
                        true
                    });
                }
            }
        }
    }

    // NPU - use CPU name for family detection
    if (devices.contains("npu") && devices["npu"].is_object()) {
        const auto& npu = devices["npu"];
        if (npu.value("available", false)) {
            std::string name = npu.value("name", "");
            detected_devices.push_back({
                "npu",
                name,
                get_device_family("npu", name, cpu_name),
                true
            });
        }
    }

    // Metal - use metal for family detection
    if (devices.contains("metal")) {
        if (devices["metal"].is_object()) {
            // Single Metal device (legacy format)
            const auto& metal = devices["metal"];
            if (metal.value("available", false)) {
                std::string name = metal.value("name", "");
                detected_devices.push_back({
                    "metal",
                    name,
                    get_device_family("metal", name, cpu_name),
                    true
                });
            }
        } else if (devices["metal"].is_array()) {
            // Multiple Metal devices
            for (const auto& metal : devices["metal"]) {
                if (metal.value("available", false)) {
                    std::string name = metal.value("name", "");
                    if (!name.empty()) {
                        detected_devices.push_back({
                            "metal",
                            name,
                            get_device_family("metal", name, cpu_name),
                            true
                        });
                    }
                }
            }
        }
    }

    // Special case: Metal is always available on macOS (system GPU)
    if (current_os == "macos" && std::find_if(detected_devices.begin(), detected_devices.end(),
        [](const DetectedDevice& d) { return d.type == "metal"; }) == detected_devices.end()) {
        detected_devices.push_back({"metal", "Apple Metal", "metal", true});
    }

    // Build recipes from the definition table
    for (const auto& def : RECIPE_DEFS) {
        // Skip if not supported on current OS
        if (def.supported_os.count(current_os) == 0) {
            // Helper to format OS name nicely
            auto format_os_name = [](const std::string& os) -> std::string {
                if (os == "macos") return "macOS";
                if (os == "windows") return "Windows";
                if (os == "linux") return "Linux";
                // Fallback: capitalize first letter
                std::string result = os;
                if (!result.empty()) result[0] = std::toupper(result[0]);
                return result;
            };

            // Generate concise OS requirement message
            std::string required_os;
            if (def.supported_os.size() == 1) {
                required_os = format_os_name(*def.supported_os.begin());
            } else {
                for (const auto& os : def.supported_os) {
                    if (!required_os.empty()) required_os += "/";
                    required_os += format_os_name(os);
                }
            }

            // Still add the recipe but mark as not supported
            json backend = {
                {"devices", json::array()},
                {"supported", false},
                {"available", false},
                {"error", "Requires " + required_os}
            };

            // Add to the appropriate recipe/backend structure
            if (recipes.contains(def.recipe)) {
                recipes[def.recipe]["backends"][def.backend] = backend;
            } else {
                recipes[def.recipe] = {{"backends", {{def.backend, backend}}}};
            }
            continue;
        }

        // Find matching devices on this system and track failures for error reporting
        json matching_devices = json::array();
        // Track missing devices with their required families for error messages
        std::vector<std::pair<std::string, std::set<std::string>>> missing_devices;  // Device types not present
        std::vector<std::pair<std::string, std::set<std::string>>> wrong_family;     // Device present but wrong family

        for (const auto& [required_device_type, required_families] : def.devices) {
            bool device_type_found = false;
            bool family_matched = false;

            for (const auto& detected : detected_devices) {
                if (detected.type == required_device_type) {
                    device_type_found = true;
                    if (device_matches_constraint(detected.family, required_families)) {
                        matching_devices.push_back(detected.type);
                        family_matched = true;
                    }
                }
            }

            if (!device_type_found) {
                missing_devices.push_back({required_device_type, required_families});
            } else if (!family_matched) {
                wrong_family.push_back({required_device_type, required_families});
            }
        }

        // Remove duplicates (e.g., multiple dGPUs of same type)
        std::set<std::string> unique_devices;
        json unique_matching = json::array();
        for (const auto& dev : matching_devices) {
            if (unique_devices.insert(dev.get<std::string>()).second) {
                unique_matching.push_back(dev);
            }
        }

        bool supported = !unique_matching.empty();
        bool available = is_recipe_installed(def.recipe, def.backend);

        json backend = {
            {"devices", unique_matching},
            {"supported", supported},
            {"available", available}
        };

        // Generate concise error message based on what failed
        if (!supported) {
            std::string error;

            if (!missing_devices.empty()) {
                // Device type not present - include required family if specified
                const auto& [device_type, required_families] = missing_devices[0];
                if (!required_families.empty()) {
                    // Show specific family requirement (e.g., "Requires XDNA2 NPU")
                    error = "Requires " + get_family_name(*required_families.begin()) + " " + get_device_type_name(device_type);
                } else {
                    // No specific family required (e.g., "Requires CPU")
                    error = "Requires " + get_device_type_name(device_type);
                }
            } else if (!wrong_family.empty()) {
                // Device present but wrong family - show required families
                const auto& [device_type, required_families] = wrong_family[0];
                if (!required_families.empty()) {
                    // Use first required family name for concise message
                    error = "Requires " + get_family_name(*required_families.begin()) + " " + get_device_type_name(device_type);
                } else {
                    error = "Incompatible " + get_device_type_name(device_type);
                }
            } else {
                error = "No compatible device";
            }

            backend["error"] = error;
        } else if (available) {
            // Add version if installed
            std::string version = get_recipe_version(def.recipe, def.backend);
            if (!version.empty() && version != "unknown") {
                backend["version"] = version;
            }
        }

        // Add to the appropriate recipe/backend structure
        if (recipes.contains(def.recipe)) {
            recipes[def.recipe]["backends"][def.backend] = backend;
        } else {
            recipes[def.recipe] = {{"backends", {{def.backend, backend}}}};
        }
    }

    return recipes;
}

SystemInfo::SupportedBackendsResult SystemInfo::get_supported_backends(const std::string& recipe) {
    SupportedBackendsResult result;
    json system_info = SystemInfoCache::get_system_info_with_cache();

    if (!system_info.contains("recipes") || !system_info["recipes"].contains(recipe)) {
        result.not_supported_error = "Recipe '" + recipe + "' not found";
        return result;
    }

    const auto& recipe_info = system_info["recipes"][recipe];
    if (!recipe_info.contains("backends")) {
        result.not_supported_error = "No backends found for recipe '" + recipe + "'";
        return result;
    }

    // Collect supported backends and capture first error (in preference order from RECIPE_DEFS)
    for (const auto& def : RECIPE_DEFS) {
        if (def.recipe == recipe) {
            if (recipe_info["backends"].contains(def.backend)) {
                const auto& backend = recipe_info["backends"][def.backend];
                if (backend.value("supported", false)) {
                    result.backends.push_back(def.backend);
                } else if (result.not_supported_error.empty() && backend.contains("error")) {
                    // Capture first error encountered (in preference order)
                    result.not_supported_error = backend["error"].get<std::string>();
                }
            }
        }
    }

    // If no backends supported and no specific error, provide generic message
    if (result.backends.empty() && result.not_supported_error.empty()) {
        result.not_supported_error = "No supported backend found for recipe '" + recipe + "'";
    }

    return result;
}

std::string SystemInfo::check_recipe_supported(const std::string& recipe) {
    auto result = get_supported_backends(recipe);
    return result.backends.empty() ? result.not_supported_error : "";
}

std::vector<SystemInfo::RecipeStatus> SystemInfo::get_all_recipe_statuses() {
    std::vector<RecipeStatus> statuses;
    json system_info = SystemInfoCache::get_system_info_with_cache();

    if (!system_info.contains("recipes") || !system_info["recipes"].is_object()) {
        return statuses;
    }

    const auto& recipes = system_info["recipes"];
    for (auto& [recipe_name, recipe_info] : recipes.items()) {
        bool any_supported = false;
        bool any_available = false;
        std::string first_error;
        std::vector<BackendStatus> backends;

        if (recipe_info.contains("backends") && recipe_info["backends"].is_object()) {
            // Iterate in preference order (from RECIPE_DEFS table)
            for (const auto& def : RECIPE_DEFS) {
                if (def.recipe != recipe_name) continue;

                if (!recipe_info["backends"].contains(def.backend)) continue;

                const auto& backend_info = recipe_info["backends"][def.backend];
                bool supported = backend_info.value("supported", false);
                bool available = backend_info.value("available", false);
                std::string version = backend_info.value("version", "");
                std::string error = backend_info.value("error", "");

                if (supported) any_supported = true;
                if (available) any_available = true;

                if (!supported && first_error.empty() && !error.empty()) {
                    first_error = error;
                }

                backends.push_back({def.backend, supported, available, version, error});
            }
        }

        statuses.push_back({recipe_name, any_supported, any_available, first_error, backends});
    }

    return statuses;
}

// Helper to read version from a version.txt file
static std::string read_version_file(const fs::path& version_file) {
    if (fs::exists(version_file)) {
        std::ifstream file(version_file);
        if (file.is_open()) {
            std::string version;
            std::getline(file, version);
            file.close();
            // Trim whitespace
            size_t start = version.find_first_not_of(" \t\n\r");
            size_t end = version.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                return version.substr(start, end - start + 1);
            }
        }
    }
    return "unknown";
}

std::string SystemInfo::get_llamacpp_version(const std::string& backend) {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    return read_version_file(bin_dir / "llama" / backend / "version.txt");
}

std::string SystemInfo::get_whispercpp_version() {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    return read_version_file(bin_dir / "whisper" / "version.txt");
}

std::string SystemInfo::get_kokoro_version(const std::string& backend) {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    return read_version_file(bin_dir / "kokoro" / backend / "version.txt");
}

std::string SystemInfo::get_sdcpp_version() {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    return read_version_file(bin_dir / "sd-cpp" / "version.txt");
}

std::string SystemInfo::get_oga_version() {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    return read_version_file(bin_dir / "ryzenai-server" / "version.txt");
}

bool SystemInfo::is_llamacpp_installed(const std::string& backend) {
    // Check if llama-server executable exists for the given backend
    // Location: {cache_dir}/bin/llama/{backend}/

    fs::path bin_dir = utils::get_downloaded_bin_dir();
    fs::path install_dir = bin_dir / "llama" / backend;

#ifdef _WIN32
    fs::path llama_exe = install_dir / "llama-server.exe";
    return fs::exists(llama_exe);
#else
    // For Linux, check build/bin subdirectory first, then root
    fs::path build_bin_path = install_dir / "build" / "bin" / "llama-server";
    if (fs::exists(build_bin_path)) {
        return true;
    }
    fs::path root_path = install_dir / "llama-server";
    return fs::exists(root_path);
#endif
}

bool SystemInfo::is_whispercpp_installed() {
    // Check if whisper-server executable exists
    // Location: {cache_dir}/bin/whisper/

    fs::path bin_dir = utils::get_downloaded_bin_dir();
    fs::path install_dir = bin_dir / "whisper";

#ifdef _WIN32
    std::vector<std::string> exe_names = {"whisper-server.exe", "server.exe"};
#else
    std::vector<std::string> exe_names = {"whisper-server", "server"};
#endif
    std::vector<std::string> subdirs = {"Release", "bin", ""};

    for (const auto& subdir : subdirs) {
        for (const auto& exe_name : exe_names) {
            fs::path exe_path;
            if (subdir.empty()) {
                exe_path = install_dir / exe_name;
            } else {
                exe_path = install_dir / subdir / exe_name;
            }
            if (fs::exists(exe_path)) {
                return true;
            }
        }
    }

    return false;
}

bool SystemInfo::is_kokoro_installed(const std::string& backend) {
    fs::path bin_dir = utils::get_downloaded_bin_dir();
    fs::path install_dir = bin_dir / "kokoro" / backend;

    // Look for kokoros executable
#ifdef _WIN32
    std::vector<std::string> exe_names = {"koko.exe"};
    std::vector<std::string> subdirs = {"kokoros-windows-x86_64", "windows-x86_64", ""};
#else
    std::vector<std::string> exe_names = {"koko"};
    std::vector<std::string> subdirs = {"kokoros-linux-x86_64", "linux-x86_64", ""};
#endif

    for (const auto& subdir : subdirs) {
        for (const auto& exe_name : exe_names) {
            fs::path exe_path;
            if (subdir.empty()) {
                exe_path = fs::path(install_dir) / exe_name;
            } else {
                exe_path = fs::path(install_dir) / subdir / exe_name;
            }
            if (fs::exists(exe_path)) {
                return true;
            }
        }
    }

    return false;
}

bool SystemInfo::is_sdcpp_installed() {
    // Check if sd-server executable exists
    // Location: {cache_dir}/bin/sd-cpp/

    fs::path bin_dir = utils::get_downloaded_bin_dir();
    fs::path install_dir = bin_dir / "sd-cpp";

#ifdef _WIN32
    fs::path exe_path = install_dir / "sd-server.exe";
#else
    fs::path exe_path = install_dir / "sd-server";
#endif

    return fs::exists(exe_path);
}

// Helper to identify ROCm architecture from GPU name
std::string identify_rocm_arch_from_name(const std::string& device_name) {
    std::string device_lower = device_name;
    std::transform(device_lower.begin(), device_lower.end(), device_lower.begin(), ::tolower);

    // linux will pass the ISA from KFD, transform it to what the rest of lemonade expects
    if (std::all_of(device_lower.begin(), device_lower.end(), ::isdigit)) {
        if (device_lower.length() >= 4) {
            std::string major = device_lower.substr(0, 2);

            int minor_int = std::stoi(device_lower.substr(2, 2));
            std::string minor = std::to_string(minor_int);

            int revision_int = std::stoi(device_lower.substr(4, 2));
            std::string revision = std::to_string(revision_int);

            std::string arch = "gfx" + major + minor + revision;

            // Apply architecture family mapping
            auto it = ROCM_ARCH_MAPPING.find(arch);
            if (it != ROCM_ARCH_MAPPING.end()) {
                return it->second;
            }

            return arch;
        }
    }

    if (device_lower.find("radeon") == std::string::npos &&
        device_lower.find("amd") == std::string::npos) {
        return "";
    }

    // STX Halo iGPUs (gfx1151 architecture)
    // Radeon 8050S Graphics / Radeon 8060S Graphics
    if (device_lower.find("8050s") != std::string::npos ||
        device_lower.find("8060s") != std::string::npos ||
        device_lower.find("device 1586") != std::string::npos) {
        return "gfx1151";
    }

    // STX Point iGPUs (gfx1150 architecture)
    // Radeon 880M / 890M Graphics
    if (device_lower.find("880m") != std::string::npos ||
        device_lower.find("890m") != std::string::npos) {
        return "gfx1150";
    }

    // RDNA4 GPUs (gfx120X architecture)
    // AMD Radeon AI PRO R9700, AMD Radeon RX 9070 XT, AMD Radeon RX 9070 GRE,
    // AMD Radeon RX 9070, AMD Radeon RX 9060 XT
    if (device_lower.find("r9700") != std::string::npos ||
        device_lower.find("9060") != std::string::npos ||
        device_lower.find("9070") != std::string::npos) {
        return "gfx120X";
    }

    // RDNA3 GPUs (gfx110X architecture)
    // AMD Radeon PRO V710, AMD Radeon PRO W7900 Dual Slot, AMD Radeon PRO W7900,
    // AMD Radeon PRO W7800 48GB, AMD Radeon PRO W7800, AMD Radeon PRO W7700,
    // AMD Radeon RX 7900 XTX, AMD Radeon RX 7900 XT, AMD Radeon RX 7900 GRE,
    // AMD Radeon RX 7800 XT, AMD Radeon RX 7700 XT
    if (device_lower.find("7700") != std::string::npos ||
        device_lower.find("7800") != std::string::npos ||
        device_lower.find("7900") != std::string::npos ||
        device_lower.find("v710") != std::string::npos) {
        return "gfx110X";
    }

    return "";
}

// Identify NPU architecture from processor name
// Returns the NPU family (e.g., "XDNA2") or empty string if not an NPU-capable processor
// This is the single source of truth for NPU family detection
std::string identify_npu_arch(const std::string& processor_name) {
    std::string processor_lower = processor_name;
    std::transform(processor_lower.begin(), processor_lower.end(), processor_lower.begin(), ::tolower);

    // Must be a Ryzen AI processor
    if (processor_lower.find("ryzen ai") == std::string::npos) {
        return "";
    }

    // XDNA2 architecture: Ryzen AI 300-series, 400-series, Z2 series
    // Pattern: "ryzen ai" followed by 3xx, 4xx, or Z2
    // Examples:
    //   - AMD Ryzen AI 9 HX 375
    //   - AMD Ryzen AI 9 365
    //   - AMD Ryzen AI 7 350
    //   - AMD Ryzen AI Max+ 395 (400-series when available)
    //   - AMD Ryzen AI Z2 Extreme
    std::regex xdna2_pattern(R"(ryzen ai.*((\b[34]\d{2}\b)|(\bz2\b)))", std::regex::icase);
    if (std::regex_search(processor_lower, xdna2_pattern)) {
        return "XDNA2";
    }

    // Future: Add XDNA3, XDNA4, etc. as new architectures are released

    return "";
}

std::string SystemInfo::get_rocm_arch() {
    // Returns the ROCm architecture for the best available AMD GPU on this system
    // Checks iGPU first, then dGPUs. Returns empty string if no compatible GPU found.
    try {
        auto system_info = create_system_info();

        // Check iGPU first
        auto igpu = system_info->get_amd_igpu_device();
        if (igpu.available && !igpu.name.empty()) {
            std::string arch = identify_rocm_arch_from_name(igpu.name);
            if (!arch.empty()) {
                return arch;
            }
        }

        // Check dGPUs
        auto dgpus = system_info->get_amd_dgpu_devices();
        for (const auto& gpu : dgpus) {
            if (gpu.available && !gpu.name.empty()) {
                std::string arch = identify_rocm_arch_from_name(gpu.name);
                if (!arch.empty()) {
                    return arch;
                }
            }
        }
    } catch (...) {
        // Detection failed
    }

    return "";  // No supported architecture found
}

std::string SystemInfo::get_flm_version() {
    #ifdef _WIN32
    FILE* pipe = _popen("flm version 2>NUL", "r");
    if (!pipe) {
        return "unknown";
    }

    char buffer[256];
    std::string output;
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output = buffer;
    }
    _pclose(pipe);

    // Parse version from output like "FLM v0.9.4"
    if (output.find("FLM v") != std::string::npos) {
        size_t pos = output.find("FLM v");
        std::string version = output.substr(pos + 5);
        // Trim whitespace and newlines
        size_t end = version.find_first_of(" \t\n\r");
        if (end != std::string::npos) {
            version = version.substr(0, end);
        }
        return version;
    }
    #endif

    return "unknown";
}

bool SystemInfo::is_ryzenai_serve_available() {
    // Inline the check to avoid dependency on RyzenAIServer class
    // 1. Check for custom binary via environment variable
    const char* ryzenai_bin_env = std::getenv("LEMONADE_RYZENAI_SERVER_BIN");
    if (ryzenai_bin_env && fs::exists(ryzenai_bin_env)) {
        return true;
    }

    // 2. Check in install directory (where download_and_install() places it)
    fs::path install_dir = fs::path(utils::get_downloaded_bin_dir()) / "ryzenai-server";
    #ifdef _WIN32
    fs::path exe_path = install_dir / "ryzenai-server.exe";
    #else
    fs::path exe_path = install_dir / "ryzenai-server";
    #endif
    return fs::exists(exe_path);
}

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<SystemInfo> create_system_info() {
    #ifdef _WIN32
    return std::make_unique<WindowsSystemInfo>();
    #elif __linux__
    return std::make_unique<LinuxSystemInfo>();
    #elif __APPLE__
    return std::make_unique<MacOSSystemInfo>();
    #else
    throw std::runtime_error("Unsupported operating system");
    #endif
}

// ============================================================================
// Windows implementation
// ============================================================================

#ifdef _WIN32

WindowsSystemInfo::WindowsSystemInfo() {
    // COM initialization handled by WMIConnection
}

CPUInfo WindowsSystemInfo::get_cpu_device() {
    CPUInfo cpu;
    cpu.available = false;

    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        cpu.error = "Failed to connect to WMI";
        return cpu;
    }

    wmi.query(L"SELECT * FROM Win32_Processor", [&cpu](IWbemClassObject* pObj) {
        cpu.name = wmi::get_property_string(pObj, L"Name");
        cpu.cores = wmi::get_property_int(pObj, L"NumberOfCores");
        cpu.threads = wmi::get_property_int(pObj, L"NumberOfLogicalProcessors");
        cpu.max_clock_speed_mhz = wmi::get_property_int(pObj, L"MaxClockSpeed");
        cpu.available = true;
    });

    if (!cpu.available) {
        cpu.error = "No CPU information found";
    }

    return cpu;
}

GPUInfo WindowsSystemInfo::get_amd_igpu_device() {
    auto gpus = detect_amd_gpus("integrated");
    if (!gpus.empty()) {
        return gpus[0];
    }

    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "No AMD integrated GPU found";
    return gpu;
}

std::vector<GPUInfo> WindowsSystemInfo::get_amd_dgpu_devices() {
    return detect_amd_gpus("discrete");
}

std::vector<GPUInfo> WindowsSystemInfo::get_nvidia_dgpu_devices() {
    std::vector<GPUInfo> gpus;

    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "Failed to connect to WMI";
        gpus.push_back(gpu);
        return gpus;
    }

    wmi.query(L"SELECT * FROM Win32_VideoController", [&gpus, this](IWbemClassObject* pObj) {
        std::string name = wmi::get_property_string(pObj, L"Name");

        // Check if this is an NVIDIA GPU
        if (name.find("NVIDIA") != std::string::npos) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            // Most NVIDIA GPUs are discrete
            bool is_discrete = true;
            for (const auto& keyword : NVIDIA_DISCRETE_GPU_KEYWORDS) {
                if (name_lower.find(keyword) != std::string::npos) {
                    is_discrete = true;
                    break;
                }
            }

            if (is_discrete) {
                GPUInfo gpu;
                gpu.name = name;
                gpu.available = true;

                // Get driver version - try multiple methods
                std::string driver_version = get_driver_version("NVIDIA");
                if (driver_version.empty()) {
                    driver_version = wmi::get_property_string(pObj, L"DriverVersion");
                }
                gpu.driver_version = driver_version.empty() ? "Unknown" : driver_version;

                // Get VRAM
                uint64_t adapter_ram = wmi::get_property_uint64(pObj, L"AdapterRAM");
                if (adapter_ram > 0) {
                    gpu.vram_gb = adapter_ram / (1024.0 * 1024.0 * 1024.0);
                }

                gpus.push_back(gpu);
            }
        }
    });

    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No NVIDIA discrete GPU found";
        gpus.push_back(gpu);
    }

    return gpus;
}

bool WindowsSystemInfo::is_supported_ryzen_ai_processor() {
    // Check if the processor has a supported NPU architecture
    // Uses identify_npu_arch() as the single source of truth

    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        // If we can't connect to WMI, we can't verify the processor
        return false;
    }

    std::string processor_name;
    wmi.query(L"SELECT * FROM Win32_Processor", [&processor_name](IWbemClassObject* pObj) {
        if (processor_name.empty()) {  // Only get first processor
            processor_name = wmi::get_property_string(pObj, L"Name");
        }
    });

    if (processor_name.empty()) {
        return false;
    }

    // Use identify_npu_arch as the single source of truth for NPU support
    std::string npu_arch = identify_npu_arch(processor_name);
    return !npu_arch.empty();
}

NPUInfo WindowsSystemInfo::get_npu_device() {
    NPUInfo npu;
    npu.name = "AMD NPU";
    npu.available = false;

    // First, check if the processor is a supported Ryzen AI processor
    if (!is_supported_ryzen_ai_processor()) {
        npu.error = "NPU requires AMD Ryzen AI 300-series processor";
        return npu;
    }

    // Check for NPU driver
    std::string driver_version = get_driver_version("NPU Compute Accelerator Device");
    if (!driver_version.empty()) {
        npu.power_mode = get_npu_power_mode();
        npu.available = true;
    } else {
        npu.error = "No NPU device found";
    }

    return npu;
}

std::vector<GPUInfo> WindowsSystemInfo::detect_amd_gpus(const std::string& gpu_type) {
    std::vector<GPUInfo> gpus;

    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "Failed to connect to WMI";
        gpus.push_back(gpu);
        return gpus;
    }

    wmi.query(L"SELECT * FROM Win32_VideoController", [&gpus, &gpu_type, this](IWbemClassObject* pObj) {
        std::string name = wmi::get_property_string(pObj, L"Name");

        // Check if this is an AMD Radeon GPU
        if (name.find("AMD") != std::string::npos && name.find("Radeon") != std::string::npos) {
            // Convert to lowercase for keyword matching
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            // Classify as discrete or integrated based on keywords
            bool is_discrete = false;
            for (const auto& keyword : AMD_DISCRETE_GPU_KEYWORDS) {
                if (name_lower.find(keyword) != std::string::npos) {
                    is_discrete = true;
                    break;
                }
            }
            bool is_integrated = !is_discrete;

            // Filter based on requested type
            if ((gpu_type == "integrated" && is_integrated) ||
                (gpu_type == "discrete" && is_discrete)) {

                GPUInfo gpu;
                gpu.name = name;
                gpu.available = true;

                // Get driver version
                gpu.driver_version = get_driver_version("AMD-OpenCL User Mode Driver");
                if (gpu.driver_version.empty()) {
                    gpu.driver_version = "Unknown";
                }

                // Get VRAM for discrete GPUs
                if (is_discrete) {
                    // Try dxdiag first (most reliable for dedicated memory)
                    double vram_gb = get_gpu_vram_dxdiag(name);

                    // Fallback to WMI if dxdiag fails
                    if (vram_gb == 0.0) {
                        uint64_t adapter_ram = wmi::get_property_uint64(pObj, L"AdapterRAM");
                        vram_gb = get_gpu_vram_wmi(adapter_ram);
                    }

                    if (vram_gb > 0.0) {
                        gpu.vram_gb = vram_gb;
                    }
                }

                gpus.push_back(gpu);
            }
        }
    });

    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No AMD " + gpu_type + " GPU found";
        gpus.push_back(gpu);
    }

    return gpus;
}

std::string WindowsSystemInfo::get_driver_version(const std::string& device_name) {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "";
    }

    std::string driver_version;
    std::wstring query = L"SELECT * FROM Win32_PnPSignedDriver WHERE DeviceName LIKE '%" +
                         wmi::string_to_wstring(device_name) + L"%'";

    wmi.query(query, [&driver_version](IWbemClassObject* pObj) {
        if (driver_version.empty()) {  // Only get first match
            driver_version = wmi::get_property_string(pObj, L"DriverVersion");
        }
    });

    return driver_version;
}

std::string WindowsSystemInfo::get_npu_power_mode() {
    // Try to query xrt-smi for NPU power mode
    std::string xrt_smi_path = "C:\\Windows\\System32\\AMD\\xrt-smi.exe";

    // Check if xrt-smi exists
    if (!fs::exists(xrt_smi_path)) {
        return "Unknown";
    }

    // Execute xrt-smi examine -r platform
    std::string command = "\"" + xrt_smi_path + "\" examine -r platform 2>NUL";

    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) {
        return "Unknown";
    }

    char buffer[128];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);

    // Parse output for "Mode" line
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("Mode") != std::string::npos) {
            // Extract the last word from the line
            size_t last_space = line.find_last_of(" \t");
            if (last_space != std::string::npos) {
                return line.substr(last_space + 1);
            }
        }
    }

    return "Unknown";
}

json WindowsSystemInfo::get_system_info_dict() {
    json info = SystemInfo::get_system_info_dict();  // Get base fields (includes OS Version)
    info["Processor"] = get_processor_name();
    info["OEM System"] = get_system_model();
    info["Physical Memory"] = get_physical_memory();
    info["BIOS Version"] = get_bios_version();
    info["CPU Max Clock"] = get_max_clock_speed();
    info["Windows Power Setting"] = get_windows_power_setting();
    return info;
}

std::string WindowsSystemInfo::get_os_version() {
    // Get detailed Windows version using WMI (similar to Python's platform.platform())
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "Windows";  // Fallback to basic name
    }

    std::string os_name, version, build_number;
    wmi.query(L"SELECT * FROM Win32_OperatingSystem", [&](IWbemClassObject* pObj) {
        if (os_name.empty()) {  // Only get first result
            os_name = wmi::get_property_string(pObj, L"Caption");
            version = wmi::get_property_string(pObj, L"Version");
            build_number = wmi::get_property_string(pObj, L"BuildNumber");
        }
    });

    if (!os_name.empty()) {
        std::string result = os_name;
        if (!version.empty()) {
            result += " " + version;
        }
        if (!build_number.empty()) {
            result += " (Build " + build_number + ")";
        }
        return result;
    }

    return "Windows";  // Fallback
}

std::string WindowsSystemInfo::get_processor_name() {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "Processor information not found.";
    }

    std::string processor_name;
    int cores = 0;
    int threads = 0;

    wmi.query(L"SELECT * FROM Win32_Processor", [&](IWbemClassObject* pObj) {
        if (processor_name.empty()) {  // Only get first processor
            processor_name = wmi::get_property_string(pObj, L"Name");
            cores = wmi::get_property_int(pObj, L"NumberOfCores");
            threads = wmi::get_property_int(pObj, L"NumberOfLogicalProcessors");
        }
    });

    if (!processor_name.empty()) {
        // Trim whitespace
        size_t start = processor_name.find_first_not_of(" \t");
        size_t end = processor_name.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
            processor_name = processor_name.substr(start, end - start + 1);
        }

        return processor_name + " (" + std::to_string(cores) + " cores, " +
               std::to_string(threads) + " logical processors)";
    }

    return "Processor information not found.";
}

std::string WindowsSystemInfo::get_physical_memory() {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "Physical memory information not found.";
    }

    uint64_t total_capacity = 0;

    wmi.query(L"SELECT * FROM Win32_PhysicalMemory", [&](IWbemClassObject* pObj) {
        uint64_t capacity = wmi::get_property_uint64(pObj, L"Capacity");
        total_capacity += capacity;
    });

    if (total_capacity > 0) {
        double gb = total_capacity / (1024.0 * 1024.0 * 1024.0);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << gb << " GB";
        return oss.str();
    }

    return "Physical memory information not found.";
}

std::string WindowsSystemInfo::get_system_model() {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "System model information not found.";
    }

    std::string model;
    wmi.query(L"SELECT * FROM Win32_ComputerSystem", [&](IWbemClassObject* pObj) {
        if (model.empty()) {  // Only get first result
            model = wmi::get_property_string(pObj, L"Model");
        }
    });

    return model.empty() ? "System model information not found." : model;
}

std::string WindowsSystemInfo::get_bios_version() {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "BIOS Version not found.";
    }

    std::string bios_version;
    wmi.query(L"SELECT * FROM Win32_BIOS", [&](IWbemClassObject* pObj) {
        if (bios_version.empty()) {  // Only get first result
            bios_version = wmi::get_property_string(pObj, L"Name");
        }
    });

    return bios_version.empty() ? "BIOS Version not found." : bios_version;
}

std::string WindowsSystemInfo::get_max_clock_speed() {
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "Max CPU clock speed not found.";
    }

    int max_clock = 0;
    wmi.query(L"SELECT * FROM Win32_Processor", [&](IWbemClassObject* pObj) {
        if (max_clock == 0) {  // Only get first processor
            max_clock = wmi::get_property_int(pObj, L"MaxClockSpeed");
        }
    });

    if (max_clock > 0) {
        return std::to_string(max_clock) + " MHz";
    }

    return "Max CPU clock speed not found.";
}

std::string WindowsSystemInfo::get_windows_power_setting() {
    // Execute powercfg /getactivescheme
    FILE* pipe = _popen("powercfg /getactivescheme 2>NUL", "r");
    if (!pipe) {
        return "Windows power setting not found (command failed)";
    }

    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    _pclose(pipe);

    // Extract power scheme name from parentheses
    // Output format: "Power Scheme GUID: ... (Power Scheme Name)"
    size_t start = result.find('(');
    size_t end = result.find(')');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        return result.substr(start + 1, end - start - 1);
    }

    return "Power scheme name not found in output";
}

double WindowsSystemInfo::get_gpu_vram_wmi(uint64_t adapter_ram) {
    if (adapter_ram > 0) {
        return adapter_ram / (1024.0 * 1024.0 * 1024.0);
    }
    return 0.0;
}

double WindowsSystemInfo::get_gpu_vram_dxdiag(const std::string& gpu_name) {
    // Get GPU VRAM using dxdiag (most reliable for dedicated memory)
    // Similar to Python's _get_gpu_vram_dxdiag_simple method

    try {
        // Create temp file path
        char temp_path[MAX_PATH];
        char temp_dir[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_dir);
        GetTempFileNameA(temp_dir, "dxd", 0, temp_path);

        // Run dxdiag /t temp_path
        std::string command = "dxdiag /t \"" + std::string(temp_path) + "\" 2>NUL";
        int result = system(command.c_str());

        if (result != 0) {
            DeleteFileA(temp_path);
            return 0.0;
        }

        // Wait a bit for dxdiag to finish writing (it can take a few seconds)
        Sleep(3000);

        // Read the file
        std::ifstream file(temp_path);
        if (!file.is_open()) {
            DeleteFileA(temp_path);
            return 0.0;
        }

        std::string line;
        bool found_gpu = false;
        double vram_gb = 0.0;

        // Convert gpu_name to lowercase for case-insensitive comparison
        std::string gpu_name_lower = gpu_name;
        std::transform(gpu_name_lower.begin(), gpu_name_lower.end(), gpu_name_lower.begin(), ::tolower);

        while (std::getline(file, line)) {
            // Convert line to lowercase for case-insensitive search
            std::string line_lower = line;
            std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);

            // Check if this is our GPU
            if (line_lower.find("card name:") != std::string::npos &&
                line_lower.find(gpu_name_lower) != std::string::npos) {
                found_gpu = true;
                continue;
            }

            // Look for dedicated memory line
            if (found_gpu && line_lower.find("dedicated memory:") != std::string::npos) {
                // Extract memory value (format: "Dedicated Memory: 12345 MB")
                std::regex memory_regex(R"((\d+(?:\.\d+)?)\s*MB)", std::regex::icase);
                std::smatch match;
                if (std::regex_search(line, match, memory_regex)) {
                    try {
                        double vram_mb = std::stod(match[1].str());
                        vram_gb = std::round(vram_mb / 1024.0 * 10.0) / 10.0;  // Convert to GB, round to 1 decimal
                        break;
                    } catch (...) {
                        // Continue searching
                    }
                }
            }

            // Reset if we hit another display device
            if (line_lower.find("card name:") != std::string::npos &&
                line_lower.find(gpu_name_lower) == std::string::npos) {
                found_gpu = false;
            }
        }

        file.close();
        DeleteFileA(temp_path);

        return vram_gb;
    } catch (...) {
        return 0.0;
    }
}

#endif // _WIN32

// ============================================================================
// Linux implementation
// ============================================================================

#ifdef __linux__

CPUInfo LinuxSystemInfo::get_cpu_device() {
    CPUInfo cpu;
    cpu.available = false;

    // Execute lscpu command
    FILE* pipe = popen("lscpu 2>/dev/null", "r");
    if (!pipe) {
        cpu.error = "Failed to execute lscpu command";
        return cpu;
    }

    char buffer[256];
    std::string lscpu_output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        lscpu_output += buffer;
    }
    pclose(pipe);

    // Parse lscpu output
    std::istringstream iss(lscpu_output);
    std::string line;
    int cores_per_socket = 0;
    int sockets = 1;  // Default to 1

    while (std::getline(iss, line)) {
        if (line.find("Model name:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                cpu.name = line.substr(pos + 1);
                // Trim whitespace
                size_t start = cpu.name.find_first_not_of(" \t");
                size_t end = cpu.name.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    cpu.name = cpu.name.substr(start, end - start + 1);
                }
                cpu.available = true;
            }
        } else if (line.find("CPU(s):") != std::string::npos && line.find("NUMA") == std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string threads_str = line.substr(pos + 1);
                cpu.threads = std::stoi(threads_str);
            }
        } else if (line.find("Core(s) per socket:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string cores_str = line.substr(pos + 1);
                cores_per_socket = std::stoi(cores_str);
            }
        } else if (line.find("Socket(s):") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string sockets_str = line.substr(pos + 1);
                sockets = std::stoi(sockets_str);
            }
        }
    }

    // Calculate total cores
    if (cores_per_socket > 0) {
        cpu.cores = cores_per_socket * sockets;
    }

    if (!cpu.available) {
        cpu.error = "No CPU information found";
        return cpu;
    }

    return cpu;
}

GPUInfo LinuxSystemInfo::get_amd_igpu_device() {
    auto gpus = detect_amd_gpus("integrated");
    if (!gpus.empty() && gpus[0].available) {
        return gpus[0];
    }

    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "No AMD integrated GPU found";
    return gpu;
}

std::vector<GPUInfo> LinuxSystemInfo::get_amd_dgpu_devices() {
    return detect_amd_gpus("discrete");
}

std::vector<GPUInfo> LinuxSystemInfo::get_nvidia_dgpu_devices() {
    std::vector<GPUInfo> gpus;

    // Execute lspci to find GPUs
    FILE* pipe = popen("lspci 2>/dev/null | grep -iE 'vga|3d|display'", "r");
    if (!pipe) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "Failed to execute lspci command";
        gpus.push_back(gpu);
        return gpus;
    }

    char buffer[512];
    std::vector<std::string> lspci_lines;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        lspci_lines.push_back(buffer);
    }
    pclose(pipe);

    // Parse NVIDIA GPUs
    for (const auto& line : lspci_lines) {
        if (line.find("NVIDIA") != std::string::npos || line.find("nvidia") != std::string::npos) {
            // Extract device name
            std::string name;
            size_t pos = line.find(": ");
            if (pos != std::string::npos) {
                name = line.substr(pos + 2);
                // Remove newline
                if (!name.empty() && name.back() == '\n') {
                    name.pop_back();
                }
            } else {
                name = line;
            }

            // Check if discrete (most NVIDIA GPUs are discrete)
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            bool is_discrete = true;  // Default to discrete for NVIDIA
            for (const auto& keyword : NVIDIA_DISCRETE_GPU_KEYWORDS) {
                if (name_lower.find(keyword) != std::string::npos) {
                    is_discrete = true;
                    break;
                }
            }

            if (is_discrete) {
                GPUInfo gpu;
                gpu.name = name;
                gpu.available = true;

                // Get driver version
                gpu.driver_version = get_nvidia_driver_version();
                if (gpu.driver_version.empty()) {
                    gpu.driver_version = "Unknown";
                }

                // Get VRAM
                double vram = get_nvidia_vram();
                if (vram > 0.0) {
                    gpu.vram_gb = vram;
                }

                gpus.push_back(gpu);
            }
        }
    }

    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No NVIDIA discrete GPU found";
        gpus.push_back(gpu);
    }

    return gpus;
}

NPUInfo LinuxSystemInfo::get_npu_device() {
    NPUInfo npu;
    npu.name = "AMD NPU";
    npu.available = false;
    npu.error = "NPU detection not yet implemented for Linux";
    return npu;
}

std::vector<GPUInfo> LinuxSystemInfo::detect_amd_gpus(const std::string& gpu_type) {
    std::vector<GPUInfo> gpus;
    std::string kfd_path = "/sys/class/kfd/kfd/topology/nodes";

    if (!fs::exists(kfd_path)) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No KFD nodes found (AMD GPU driver not loaded or no GPU present)";
        gpus.push_back(gpu);
        return gpus;
    }

    for (const auto& node_entry : fs::directory_iterator(kfd_path)) {
        if (!node_entry.is_directory()) continue;

        std::string node_path = node_entry.path().string();
        std::string properties_file = node_path + "/properties";

        if (!fs::exists(properties_file)) continue;

        std::ifstream props(properties_file);
        if (!props.is_open()) continue;

        std::string line;
        std::string drm_render_minor;
        std::string gfx_target_version;

        bool is_gpu = false;

        while (std::getline(props, line)) {
            if (line.find("gfx_target_version") == 0) {
                gfx_target_version = line.substr(line.find(" ") + 1);
                gfx_target_version.erase(gfx_target_version.find_last_not_of(" \t\n\r") + 1);
                if (!gfx_target_version.empty() && std::stoi(gfx_target_version) != 0) {
                    is_gpu = true;
                }
            } else if (line.find("drm_render_minor") == 0) {
                drm_render_minor = line.substr(line.find(" ") + 1);
                drm_render_minor.erase(drm_render_minor.find_last_not_of(" \t\n\r") + 1);
            }
        }
        props.close();

        if (!is_gpu || drm_render_minor.empty() || drm_render_minor == "-1")
            continue;

        bool is_integrated = get_amd_is_igpu(drm_render_minor);
        if ((gpu_type == "integrated" && !is_integrated) || (gpu_type == "discrete" && is_integrated)) continue;

        GPUInfo gpu;
        gpu.name = gfx_target_version;
        gpu.available = true;

        // Get VRAM and GTT for GPUs
        gpu.vram_gb = get_amd_vram(drm_render_minor);
        gpu.virtual_gb = get_amd_gtt(drm_render_minor);

        gpus.push_back(gpu);
    }

    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No AMD " + gpu_type + " GPU found in KFD nodes";
        gpus.push_back(gpu);
    }

    return gpus;
}

std::string LinuxSystemInfo::get_nvidia_driver_version() {
    // Try nvidia-smi first
    FILE* pipe = popen("nvidia-smi --query-gpu=driver_version --format=csv,noheader,nounits 2>/dev/null", "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string version = buffer;
            // Remove newline
            if (!version.empty() && version.back() == '\n') {
                version.pop_back();
            }
            pclose(pipe);
            if (!version.empty() && version != "N/A") {
                return version;
            }
        }
        pclose(pipe);
    }

    // Fallback: Try /proc/driver/nvidia/version
    std::ifstream file("/proc/driver/nvidia/version");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            // Look for "Kernel Module  XXX.XX.XX"
            if (line.find("Kernel Module") != std::string::npos) {
                std::regex version_regex(R"(Kernel Module\s+(\d+\.\d+(?:\.\d+)?))");
                std::smatch match;
                if (std::regex_search(line, match, version_regex)) {
                    return match[1].str();
                }
            }
        }
    }

    return "";
}

double LinuxSystemInfo::get_nvidia_vram() {
    FILE* pipe = popen("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!pipe) {
        return 0.0;
    }

    char buffer[128];
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string vram_str = buffer;
        pclose(pipe);

        try {
            // nvidia-smi returns MB
            double vram_mb = std::stod(vram_str);
            return std::round(vram_mb / 1024.0 * 10.0) / 10.0;  // Convert to GB, round to 1 decimal
        } catch (...) {
            return 0.0;
        }
    }
    pclose(pipe);

    return 0.0;
}

double LinuxSystemInfo::get_ttm_gb() {
    std::string ttm_path = "/sys/module/ttm/parameters/pages_limit";
    std::ifstream sysfs_file(ttm_path);

    if (!sysfs_file.is_open()) {
        return 0.0;
    }

    std::string page_limit_str;
    std::getline(sysfs_file, page_limit_str);
    sysfs_file.close();

    try {
        uint64_t page_limit = std::stoull(page_limit_str);
        return std::round(page_limit / ((1024.0 * 1024.0 * 1024.0) / 4096) * 10.0) / 10.0;
    } catch (...) {
        return 0.0;
    }
}

bool LinuxSystemInfo::get_amd_is_igpu(const std::string& drm_render_minor) {
    std::string device_path = "/sys/class/drm/renderD" + drm_render_minor + "/device/";
    std::string board_info_path = device_path + "board_info";
    return !(fs::exists(board_info_path) && fs::is_regular_file(board_info_path));
}

double LinuxSystemInfo::parse_memory_sysfs(const std::string& drm_render_minor, const std::string& fname){
    // Try device-specific path first
    std::string sysfs_path = "/sys/class/drm/renderD" + drm_render_minor + "/device/" + fname;

    if (!fs::exists(sysfs_path))
        return 0.0;

    std::ifstream sysfs_file(sysfs_path);
    std::string memory_str;
    std::getline(sysfs_file, memory_str);
    sysfs_file.close();

    try {
        uint64_t memory_bytes = std::stoull(memory_str);
        return std::round(memory_bytes / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0;
    } catch (...) {
        return 0.0;
    }
}

double LinuxSystemInfo::get_amd_gtt(const std::string& drm_render_minor){
    return parse_memory_sysfs(drm_render_minor, "mem_info_gtt_total");
}

double LinuxSystemInfo::get_amd_vram(const std::string& drm_render_minor) {
    return parse_memory_sysfs(drm_render_minor, "mem_info_vram_total");
}

json LinuxSystemInfo::get_system_info_dict() {
    json info = SystemInfo::get_system_info_dict();  // Get base fields
    info["Processor"] = get_processor_name();
    info["Physical Memory"] = get_physical_memory();
    return info;
}

std::string LinuxSystemInfo::get_os_version() {
    // Get detailed Linux version (similar to Python's platform.platform())
    std::string result = "Linux";

    // Get kernel version from /proc/version
    std::string kernel = "unknown_kernel";

    std::ifstream file("/proc/version");
    if (file.is_open()){
        std::string line;
        std::getline(file, line);

        const std::string tag = "version ";
        size_t pos = line.find(tag);
        if (pos != std::string::npos){
            pos += tag.size();
            size_t end = line.find(' ', pos);
            kernel = line.substr(pos, end - pos);
        }
    }
    result += "-" + kernel;

    // Try to get distribution info from /etc/os-release
    std::ifstream os_release("/etc/os-release");
    if (os_release.is_open()) {
        std::string line;
        std::string distro_name, distro_version;
        while (std::getline(os_release, line)) {
            if (line.find("NAME=") == 0) {
                distro_name = line.substr(5);
                // Remove quotes
                distro_name.erase(std::remove(distro_name.begin(), distro_name.end(), '"'), distro_name.end());
            } else if (line.find("VERSION_ID=") == 0) {
                distro_version = line.substr(11);
                // Remove quotes
                distro_version.erase(std::remove(distro_version.begin(), distro_version.end(), '"'), distro_version.end());
            }
        }

        if (!distro_name.empty()) {
            result += " (" + distro_name;
            if (!distro_version.empty()) {
                result += " " + distro_version;
            }
            result += ")";
        }
    }

    return result;
}

std::string LinuxSystemInfo::get_processor_name() {
    FILE* pipe = popen("lscpu 2>/dev/null", "r");
    if (!pipe) {
        return "ERROR - Failed to execute lscpu";
    }

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);

    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("Model name:") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string name = line.substr(pos + 1);
                // Trim whitespace
                size_t start = name.find_first_not_of(" \t");
                size_t end = name.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    return name.substr(start, end - start + 1);
                }
            }
        }
    }

    return "ERROR - Processor name not found";
}

std::string LinuxSystemInfo::get_physical_memory() {
    std::string token;
    std::ifstream file("/proc/meminfo");

    //Step through each token in the file
    while(file >> token) {
        if(token == "MemTotal:") {
            // Get the token after "MemTotal:"
            if(double mem; file >> mem) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2) << std::round(mem / 1024.0 / 1024.0 * 100.0) / 100.0 << " GB";
                return oss.str();
            }
            break;
        }
        // Skip the line if key/token isn't found.
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return "ERROR - Physical memory not found";
}

#endif // __linux__

// ============================================================================
// macOS implementation
// ============================================================================

#ifdef __APPLE__

CPUInfo MacOSSystemInfo::get_cpu_device() {
    CPUInfo cpu;
    cpu.available = false;
    
    // Initialize numeric values to -1 to distinguish between "0" and "Failed to fetch"
    cpu.cores = -1;
    cpu.threads = -1;
    cpu.max_clock_speed_mhz = 0;

    size_t size;
    char buffer[256];

    size = sizeof(buffer);
    if (sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) {
        cpu.name = buffer;
        cpu.available = true;
    } else {
        cpu.name = "Unknown Apple Processor";
        cpu.error = "sysctl failed for machdep.cpu.brand_string";
    }

    int cores = 0;
    size = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0) {
        cpu.cores = cores;
    } else {
        cpu.error += " | Failed to get physical cores";
    }

    int threads = 0;
    size = sizeof(threads);
    if (sysctlbyname("hw.logicalcpu", &threads, &size, nullptr, 0) == 0) {
        cpu.threads = threads;
    } else {
        cpu.error += " | Failed to get logical threads";
    }

    // 4. Get Max Clock Speed
    uint64_t freq = 0;
    size = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency_max", &freq, &size, nullptr, 0) == 0) {
        //Calculation of hz to mhz
        cpu.max_clock_speed_mhz = (freq > 0) ? (uint32_t)(freq / 1000000) : 0;
    } else {
        cpu.error += " | Failed to get maximum frequency";
    }

    return cpu;
}

GPUInfo MacOSSystemInfo::get_amd_igpu_device() {
    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "AMD integrated GPUs not detected on macOS";
    return gpu;
}

std::vector<GPUInfo> MacOSSystemInfo::get_amd_dgpu_devices() {
    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "AMD discrete GPUs not detected on macOS";
    return {gpu};
}

std::vector<GPUInfo> MacOSSystemInfo::get_nvidia_dgpu_devices() {
    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "NVIDIA GPUs not detected on macOS";
    return {gpu};
}

NPUInfo MacOSSystemInfo::get_npu_device() {
    NPUInfo npu;
    npu.name = "AMD NPU";
    npu.available = false;
    npu.error = "NPU not supported on macOS (Ryzen AI NPUs are Windows/Linux only)";
    return npu;
}



#endif // __APPLE__

// ============================================================================
// Cache implementation
// ============================================================================

SystemInfoCache::SystemInfoCache() {
    cache_file_path_ = get_cache_dir() + "/hardware_info.json";
}

std::string SystemInfoCache::get_lemonade_version() const {
    return LEMON_VERSION_STRING;
}

bool SystemInfoCache::is_ci_mode() const {
    const char* ci_mode = std::getenv("LEMONADE_CI_MODE");
    return ci_mode != nullptr;
}

bool SystemInfoCache::is_version_less_than(const std::string& v1, const std::string& v2) {
    // Parse semantic versions (e.g., "9.0.0" vs "8.5.3")
    // Returns true if v1 < v2

    auto parse_version = [](const std::string& v) -> std::vector<int> {
        std::vector<int> parts;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.')) {
            try {
                parts.push_back(std::stoi(part));
            } catch (...) {
                parts.push_back(0);
            }
        }
        // Ensure at least 3 parts (major.minor.patch)
        while (parts.size() < 3) {
            parts.push_back(0);
        }
        return parts;
    };

    std::vector<int> parts1 = parse_version(v1);
    std::vector<int> parts2 = parse_version(v2);

    // Compare major, minor, patch (use min with parentheses to avoid Windows macro conflict)
    size_t min_size = (parts1.size() < parts2.size()) ? parts1.size() : parts2.size();
    for (size_t i = 0; i < min_size; ++i) {
        if (parts1[i] < parts2[i]) return true;
        if (parts1[i] > parts2[i]) return false;
    }

    // Equal versions
    return false;
}

bool SystemInfoCache::is_valid() const {
    // Cache is invalid in CI mode
    if (is_ci_mode()) {
        return false;
    }

    // Check if cache file exists
    if (!fs::exists(cache_file_path_)) {
        return false;
    }

    // Load cache and check version
    try {
        std::ifstream file(cache_file_path_);
        json cache_data = json::parse(file);

        // Check if version field is missing or hardware field is missing
        if (!cache_data.contains("version") || !cache_data.contains("hardware")) {
            return false;
        }

        // Get cached version and current version
        std::string cached_version = cache_data["version"];
        std::string current_version = get_lemonade_version();

        // Invalidate if cache version is less than current version
        if (is_version_less_than(cached_version, current_version)) {
            return false;
        }

        // Cache is valid
        return true;

    } catch (...) {
        return false;
    }
}

json SystemInfoCache::load_hardware_info() {
    if (!is_valid()) {
        return json::object();
    }

    try {
        std::ifstream file(cache_file_path_);
        json cache_data = json::parse(file);
        return cache_data["hardware"];
    } catch (...) {
        return json::object();
    }
}

void SystemInfoCache::save_hardware_info(const json& hardware_info) {
    // Create cache directory if it doesn't exist
    fs::create_directories(fs::path(cache_file_path_).parent_path());

    json cache_data;
    cache_data["version"] = get_lemonade_version();
    cache_data["hardware"] = hardware_info;

    std::ofstream file(cache_file_path_);
    file << cache_data.dump(2);
}

void SystemInfoCache::clear() {
    if (fs::exists(cache_file_path_)) {
        fs::remove(cache_file_path_);
    }
}

json SystemInfoCache::get_system_info_with_cache() {
    // In-memory static cache to avoid repeated disk reads and message printing
    // within the same process lifetime
    static json cached_result;
    static bool result_computed = false;

    // Return cached result if available
    if (result_computed) {
        return cached_result;
    }

    json system_info;

    // Top-level try-catch to ensure system info collection NEVER crashes Lemonade
    try {
        // Create cache instance and load cached data
        SystemInfoCache cache;
        bool cache_exists = fs::exists(cache.get_cache_file_path());
        json cached_data = cache.load_hardware_info();

        // Create platform-specific system info instance
        auto sys_info = create_system_info();

        if (!cached_data.empty()) {
            system_info = cached_data;
        } else {
            // Provide friendly message about why we're detecting hardware
            if (cache_exists) {
                std::cout << "[Server] Collecting system info (Lemonade was updated)" << std::endl;
            } else {
                std::cout << "[Server] Collecting system info" << std::endl;
            }

            // Get system info (OS, Processor, Memory, OEM System, BIOS, etc.)
            try {
                system_info = sys_info->get_system_info_dict();
            } catch (...) {
                system_info["OS Version"] = "Unknown";
            }

            // Get device information - handles its own exceptions internally
            system_info["devices"] = sys_info->get_device_dict();

            // Save to cache (without recipes which are always fresh)
            try {
                cache.save_hardware_info(system_info);
            } catch (...) {
                // Cache save failed - not critical, continue
            }
        }

        // Add recipes section (always fresh, never cached)
        system_info["recipes"] = sys_info->build_recipes_info(system_info["devices"]);

    } catch (const std::exception& e) {
        // Catastrophic failure - return minimal info but don't crash
        std::cerr << "[Server] System info failed: " << e.what() << std::endl;
        system_info = {
            {"OS Version", "Unknown"},
            {"error", e.what()},
            {"devices", json::object()}
        };
    } catch (...) {
        std::cerr << "[Server] System info failed with unknown error" << std::endl;
        system_info = {
            {"OS Version", "Unknown"},
            {"error", "Unknown error"},
            {"devices", json::object()}
        };
    }

    // Store in static cache for future calls
    cached_result = system_info;
    result_computed = true;

    return system_info;
}


} // namespace lemon
