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
    json inference_engines;
};

struct CPUInfo : DeviceInfo {
    int cores = 0;
    int threads = 0;
    int max_clock_speed_mhz = 0;
};

struct GPUInfo : DeviceInfo {
    std::string driver_version;
    double vram_gb = 0.0;
};

struct NPUInfo : DeviceInfo {
    std::string driver_version;
    std::string power_mode;
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
    static std::vector<std::string> get_python_packages();
    
    // Helper to detect inference engines for a device (public so it can be called after loading from cache)
    static json detect_inference_engines(const std::string& device_type, const std::string& device_name);
    
protected:
    // Helper methods for version detection
    static std::string get_llamacpp_version(const std::string& backend);
    static bool is_llamacpp_installed(const std::string& backend);
    static bool check_vulkan_support();
    static bool check_rocm_support(const std::string& device_name);
    static std::string get_flm_version();
    static bool is_ryzenai_serve_available();
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

private:
    std::vector<GPUInfo> detect_amd_gpus(const std::string& gpu_type);
    std::string get_nvidia_driver_version();
    double get_nvidia_vram();
    double get_amd_vram_rocm_smi();
    double get_amd_vram_sysfs(const std::string& pci_id);
};

// macOS implementation (basic stub for now)
class MacOSSystemInfo : public SystemInfo {
public:
    CPUInfo get_cpu_device() override;
    GPUInfo get_amd_igpu_device() override;
    std::vector<GPUInfo> get_amd_dgpu_devices() override;
    std::vector<GPUInfo> get_nvidia_dgpu_devices() override;
    NPUInfo get_npu_device() override;
};

// Factory function
std::unique_ptr<SystemInfo> create_system_info();

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
    static json get_system_info_with_cache(bool verbose = false);
    
private:
    std::string cache_file_path_;
    std::string get_cache_dir() const;
    std::string get_lemonade_version() const;
    bool is_ci_mode() const;
    
    // Helper to compare semantic versions (returns true if v1 < v2)
    static bool is_version_less_than(const std::string& v1, const std::string& v2);
};

} // namespace lemon

