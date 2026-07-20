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
    int index = -1;  // NVIDIA only: physical device index from nvidia-smi, when available
    std::string uuid;  // NVIDIA only: stable GPU UUID from nvidia-smi (preferred for CUDA_VISIBLE_DEVICES)
    std::string driver_version;
    std::string compute_capability;  // NVIDIA only: "MAJOR.MINOR" from nvidia-smi (e.g. "8.6")
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
    virtual std::vector<GPUInfo> get_nvidia_gpu_devices() = 0;
    virtual NPUInfo get_npu_device() = 0;

    // Apple Silicon unified-memory GPU. Only meaningful on macOS; the base
    // implementation reports "unavailable" so non-Apple platforms need no stub.
    virtual GPUInfo get_apple_silicon_device() { return GPUInfo{}; }

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

    // Device support detection
    static std::string get_rocm_arch();
    static std::string get_cuda_arch();

    // Picks the ROCm compute target from an "amd_gpu" device array: a discrete GPU wins
    // over an integrated one on a hybrid host (e.g. Strix Halo APU + MI300X dGPU).
    static std::string select_rocm_arch(const json& amd_gpu_devices);

    // Collapse a concrete ROCm ISA (e.g. gfx1201) to the family target name the
    // GitHub release repos publish their assets under (e.g. gfx120X), per the
    // rocm_asset_families map in backend_versions.json. ISAs absent from the map
    // (and values already in family form) are returned unchanged.
    static std::string rocm_asset_family(const std::string& arch);

    // AMD ships CDNA-dcgpu (gfx942) vLLM wheels on a different cadence than RDNA, so no
    // single tag covers both; returns vllm.rocm_arch_overrides[family], or empty for the
    // default pin. Beware: vLLM release tags use the raw ISA (gfx942), unlike
    // therock.url_mapping, which maps it to gfx94X-dcgpu.
    static std::string vllm_rocm_version_override(const std::string& asset_family);

    // When set non-empty on the calling thread, get_rocm_arch() returns this
    // value instead of probing hardware, so backend asset URLs can be resolved
    // for an arbitrary GPU topology with no GPU present. Per-thread so it cannot
    // affect concurrent requests.
    static void set_rocm_arch_override(const std::string& arch);

    // True if (recipe, backend) is published for the given ROCm family/ISA, per
    // the backend support matrix. Lets callers tell "this arch should have an
    // asset" from "this arch is intentionally not built" without duplicating the
    // matrix.
    static bool backend_supports_arch(const std::string& recipe,
                                      const std::string& backend,
                                      const std::string& arch);

    // CUDA release assets are architecture-specific (sm_89, sm_120, etc.).
    // Return the physical CUDA device indices whose compute capability matches
    // the selected release architecture, so callers can hide incompatible GPUs
    // with CUDA_VISIBLE_DEVICES while still using all GPUs of the same arch.
    static std::vector<int> get_cuda_device_indices_for_arch(const std::string& arch);
    static std::string get_cuda_visible_devices_for_arch(const std::string& arch);

    // Detect if the device is an iGPU
    static bool get_has_igpu();

    // Generate human-readable error message for unsupported backend
    static std::string get_unsupported_backend_error(const std::string& recipe, const std::string& backend);

    // Check if the process is running under systemd
    static bool is_running_under_systemd();

    // Global GPU memory pressure across all processes (used/total in [0,1]),
    // or -1.0 if no source is available. Used by the dynamic VRAM eviction engine.
    static double get_global_vram_usage_pct();
};

// Windows implementation
class WindowsSystemInfo : public SystemInfo {
public:
    WindowsSystemInfo();
    ~WindowsSystemInfo() override = default;

    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_gpu_devices() override;
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
    double get_nvidia_vram_smi();

    // dxdiag lists every GPU in one invocation, so we run it once and
    // serve subsequent lookups from memory.
    bool dxdiag_cache_loaded_ = false;
    std::vector<std::pair<std::string, double>> dxdiag_vram_cache_;  // (card_name_lower, vram_gb)
    void load_dxdiag_cache();
};

// Linux implementation
class LinuxSystemInfo : public SystemInfo {
public:
    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_gpu_devices() override;
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
    std::vector<GPUInfo> get_nvidia_gpu_devices() override;
    NPUInfo get_npu_device() override;
    GPUInfo get_apple_silicon_device() override;

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

// Helper to identify CUDA Compute Capability from a marketing GPU name
// Returns an sm_XX token (e.g., "sm_75", "sm_86", "sm_120") or empty string if not recognized
std::string identify_cuda_arch_from_name(const std::string& device_name);

// Check if kernel has CWSR fix for Strix Halo
bool needs_gfx1151_cwsr_fix();

// FLM status (derived from system-info cache)
struct FlmStatus {
    std::string state;     // "unsupported","installable","update_required","action_required","installed","update_available"
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
