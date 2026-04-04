#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Device information structures
struct DeviceInfo {
    std::string name;
    bool available = false;
    std::string error;
};

struct CPUInfo : DeviceInfo {
    int cores = 0;
    int threads = 0;
    int max_clock_speed_mhz = 0;
};

struct GPUInfo : DeviceInfo {
    std::string driver_version;
    double vram_gb = 0.0;
    double virtual_gb = 0.0;
};

struct NPUInfo : DeviceInfo {
    std::string driver_version;
    std::string power_mode;
    uint64_t tops_max = 0;
    uint64_t tops_curr = 0;
    float utilization = 0.0f;
};

//Enums

enum class MemoryAllocBehavior
{ // Example: VRAM=1, GTT=2, Both=3, Largest
    Hardware = 1,
    Virtual = 2,
    Unified = 3,
    Largest = 4,
};

// Base class for system information
class SystemInfo {
public:
    virtual ~SystemInfo() = default;

    // Get all system information
    virtual json get_system_info_dict();

    // Get all device information
    json get_device_dict();

    // Hardware detection methods (to be implemented by OS-specific subclasses)
    virtual CPUInfo get_cpu_device() = 0;
    virtual GPUInfo get_amd_igpu_device() = 0;
    virtual std::vector<GPUInfo> get_amd_dgpu_devices() = 0;
    virtual std::vector<GPUInfo> get_nvidia_dgpu_devices() = 0;
    virtual NPUInfo get_npu_device() = 0;

    // Common methods (can be overridden for detailed platform info)
    virtual std::string get_os_version();

    // Build the recipes section for system_info using pre-collected device info
    json build_recipes_info(const json& devices);

    // Result of checking supported backends for a recipe
    struct SupportedBackendsResult {
        std::vector<std::string> backends;  // Supported backends in preference order
        std::string not_supported_error;    // Error message if no backends are supported
    };

    // Get list of supported backends for a recipe (in preference order)
    static SupportedBackendsResult get_supported_backends(const std::string& recipe);

    // Check if a recipe is supported on the current system
    // Returns empty string if supported, or a reason string if not supported
    static std::string check_recipe_supported(const std::string& recipe);

    // Get all recipes with their backend state info
    // Returns a vector of {recipe_name, backends}
    struct BackendStatus {
        std::string name;
        std::string state;
        std::string version;
        std::string message;
        std::string action;
    };
    struct RecipeStatus {
        std::string name;
        std::vector<BackendStatus> backends;
    };
    static std::vector<RecipeStatus> get_all_recipe_statuses();

    static std::string get_flm_version();
    static std::string get_system_llamacpp_version();

    // Device support detection
    static std::string get_rocm_arch();

    // Detect if the device is an iGPU
    static bool get_has_igpu();

    // Generate human-readable error message for unsupported backend
    static std::string get_unsupported_backend_error(const std::string& recipe, const std::string& backend);

    // Check if the process is running under systemd
    static bool is_running_under_systemd();
};

// Windows implementation
class WindowsSystemInfo : public SystemInfo {
public:
    WindowsSystemInfo();
    ~WindowsSystemInfo() override = default;

    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_dgpu_devices() override;
    NPUInfo get_npu_device() override;

    // Override to add Windows-specific fields
    json get_system_info_dict() override;
    std::string get_os_version() override;

    // Windows-specific methods
    std::string get_processor_name();
    std::string get_physical_memory();

    // POD used by read_cpu_hardware() (defined in system_info.cpp)
    struct CpuHardware {
        std::string brand;
        int logical  = 0;
        int physical = 0;
    };

private:
    std::vector<GPUInfo> detect_amd_gpus(const std::string& gpu_type);
    std::string get_driver_version(const std::string& device_name);
    double get_gpu_vram_dxdiag(const std::string& gpu_name);
    double get_gpu_vram_wmi(uint64_t adapter_ram);
};

// Linux implementation
class LinuxSystemInfo : public SystemInfo {
public:
    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_dgpu_devices() override;
    NPUInfo get_npu_device() override;

    // Override to add Linux-specific fields
    json get_system_info_dict() override;
    std::string get_os_version() override;

    // Linux-specific methods
    std::string get_processor_name();
    std::string get_physical_memory();
    double get_ttm_gb();

private:
    std::vector<GPUInfo> detect_amd_gpus(const std::string& gpu_type);
    std::string get_nvidia_driver_version();
    double get_nvidia_vram();
    double get_amd_vram(const std::string& drm_render_minor);
    double get_amd_gtt(const std::string& drm_render_minor);
    bool get_amd_is_igpu(const std::string& drm_render_minor);

private:
    double parse_memory_sysfs(const std::string& drm_render_minor, const std::string& fname);
};

// macOS implementation
class MacOSSystemInfo : public SystemInfo {
public:
    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_dgpu_devices() override;
    NPUInfo get_npu_device() override;

    // Override to add macOS-specific fields
    json get_system_info_dict() override;
    std::string get_os_version() override;

    // macOS-specific methods
    std::string get_processor_name();
    std::string get_physical_memory();

    std::vector<GPUInfo> detect_metal_gpus();
};

// Factory function
std::unique_ptr<SystemInfo> create_system_info();

// Helper to identify ROCm architecture from GPU name
// Returns architecture string (e.g., "gfx1150", "gfx1151", "gfx110X", "gfx120X") or empty string if not recognized
std::string identify_rocm_arch_from_name(const std::string& device_name);

// Check if kernel has CWSR fix for Strix Halo
bool needs_gfx1151_cwsr_fix();

// FLM status (derived from system-info cache)
struct FlmStatus {
    std::string state;     // "unsupported","installable","update_required","action_required","installed"
    std::string version;
    std::string message;
    std::string action;

    bool is_ready() const { return state == "installed"; }

    // Format a user-facing error message (for throwing when FLM is not ready)
    std::string error_string() const {
        std::string msg = "FLM is not ready: " + message;
        if (!action.empty()) {
            msg += ". " + action;
        }
        return msg;
    }
};

// In-memory system info cache (populated once on first access, held for process lifetime)
class SystemInfoCache {
public:
    // Get complete system info (hardware + recipes). Computed once, then cached in memory.
    static json get_system_info_with_cache();

    // Invalidate recipes portion of cache so the next get_system_info_with_cache()
    // re-evaluates backend availability (call after installing/upgrading a backend).
    static void invalidate_recipes();

    // Get FLM status from cached system-info (single source of truth)
    static FlmStatus get_flm_status();
};

} // namespace lemon
