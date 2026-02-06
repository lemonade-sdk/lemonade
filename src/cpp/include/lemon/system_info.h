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

    // Get all recipes with their support status
    // Returns a vector of {recipe_name, supported, available, error_message, backends}
    struct BackendStatus {
        std::string name;
        bool supported;
        bool available;
        std::string version;
        std::string error;
    };
    struct RecipeStatus {
        std::string name;
        bool supported;
        bool available;
        std::string error;
        std::vector<BackendStatus> backends;
    };
    static std::vector<RecipeStatus> get_all_recipe_statuses();

    // Version and installation detection (public for use by backends and helpers)
    static std::string get_llamacpp_version(const std::string& backend);
    static std::string get_whispercpp_version(const std::string& backend);
    static std::string get_kokoro_version(const std::string& backend);
    static std::string get_sdcpp_version(const std::string& backend);
    static std::string get_oga_version();
    static std::string get_flm_version();
    static bool is_llamacpp_installed(const std::string& backend);
    static bool is_whispercpp_installed(const std::string& backend);
    static bool is_kokoro_installed(const std::string& backend);
    static bool is_sdcpp_installed(const std::string& backend);
    static bool is_ryzenai_serve_available();

    // Device support detection
    static std::string get_rocm_arch();

    // Generate human-readable error message for unsupported backend
    static std::string get_unsupported_backend_error(const std::string& recipe, const std::string& backend);
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
    std::string get_system_model();
    std::string get_bios_version();
    std::string get_max_clock_speed();
    std::string get_windows_power_setting();

private:
    std::vector<GPUInfo> detect_amd_gpus(const std::string& gpu_type);
    std::string get_driver_version(const std::string& device_name);
    std::string get_npu_power_mode();
    double get_gpu_vram_dxdiag(const std::string& gpu_name);
    double get_gpu_vram_wmi(uint64_t adapter_ram);
    bool is_supported_ryzen_ai_processor();
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

    std::vector<GPUInfo> detect_metal_gpus();
};

// Factory function
std::unique_ptr<SystemInfo> create_system_info();

// Helper to identify ROCm architecture from GPU name
// Returns architecture string (e.g., "gfx1150", "gfx1151", "gfx110X", "gfx120X") or empty string if not recognized
std::string identify_rocm_arch_from_name(const std::string& device_name);

// Cache management
class SystemInfoCache {
public:
    SystemInfoCache();

    // Check if cache is valid
    bool is_valid() const;

    // Load cached hardware info
    json load_hardware_info();

    // Save hardware info to cache
    void save_hardware_info(const json& hardware_info);

    // Clear cache
    void clear();

    // Get cache file path
    std::string get_cache_file_path() const { return cache_file_path_; }

    // High-level function: Get complete system info (with cache handling and friendly messages)
    static json get_system_info_with_cache();

private:
    std::string cache_file_path_;
    std::string get_lemonade_version() const;
    bool is_ci_mode() const;

    // Helper to compare semantic versions (returns true if v1 < v2)
    static bool is_version_less_than(const std::string& v1, const std::string& v2);
};

} // namespace lemon
