#include "lemon/system_info.h"
#include "lemon/version.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include "utils/wmi_helper.h"
#pragma comment(lib, "wbemuuid.lib")
#endif

namespace lemon {

namespace fs = std::filesystem;

// AMD discrete GPU keywords
const std::vector<std::string> AMD_DISCRETE_GPU_KEYWORDS = {
    "rx ", "xt", "pro w", "pro v", "radeon pro", "firepro", "fury"
};

// NVIDIA discrete GPU keywords  
const std::vector<std::string> NVIDIA_DISCRETE_GPU_KEYWORDS = {
    "geforce", "rtx", "gtx", "quadro", "tesla", "titan",
    "a100", "a40", "a30", "a10", "a6000", "a5000", "a4000", "a2000"
};

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

std::vector<std::string> SystemInfo::get_python_packages() {
    // Not applicable for C++ implementation
    return {"not-applicable"};
}

json SystemInfo::detect_inference_engines(const std::string& device_type, const std::string& device_name) {
    json engines;
    
    // llamacpp-vulkan: Available for CPU, AMD iGPU, AMD dGPU, NVIDIA dGPU (NOT NPU)
    if (device_type == "cpu" || device_type == "amd_igpu" || 
        device_type == "amd_dgpu" || device_type == "nvidia_dgpu") {
        
        bool device_supported = (device_type == "cpu") || check_vulkan_support();
        
        if (!device_supported) {
            engines["llamacpp-vulkan"] = {{"available", false}, {"error", "vulkan not available"}};
        } else if (!is_llamacpp_installed("vulkan")) {
            engines["llamacpp-vulkan"] = {{"available", false}, {"error", "vulkan binaries not installed"}};
        } else {
            engines["llamacpp-vulkan"] = {
                {"available", true},
                {"version", get_llamacpp_version("vulkan")},
                {"backend", "vulkan"}
            };
        }
    }
    
    // llamacpp-rocm: Available for AMD iGPU and AMD dGPU only
    if (device_type == "amd_igpu" || device_type == "amd_dgpu") {
        bool device_supported = check_rocm_support(device_name);
        
        if (!device_supported) {
            engines["llamacpp-rocm"] = {{"available", false}, {"error", "rocm not available"}};
        } else if (!is_llamacpp_installed("rocm")) {
            engines["llamacpp-rocm"] = {{"available", false}, {"error", "rocm binaries not installed"}};
        } else {
            engines["llamacpp-rocm"] = {
                {"available", true},
                {"version", get_llamacpp_version("rocm")},
                {"backend", "rocm"}
            };
        }
    }
    
    // FLM: Only available for NPU (Windows only)
    if (device_type == "npu") {
        #ifdef _WIN32
        bool flm_available = false;
        
        // Check common Windows locations
        for (const auto& path : {"C:\\Program Files\\AMD\\FLM\\flm.exe",
                                  "C:\\Program Files (x86)\\AMD\\FLM\\flm.exe"}) {
            if (fs::exists(path)) {
                flm_available = true;
                break;
            }
        }
        
        // Check if flm is in PATH
        if (!flm_available) {
            FILE* pipe = _popen("where flm 2>NUL", "r");
            if (pipe) {
                char buffer[256];
                flm_available = (fgets(buffer, sizeof(buffer), pipe) != nullptr);
                _pclose(pipe);
            }
        }
        
        engines["flm"] = {
            {"available", flm_available},
            {"version", flm_available ? get_flm_version() : "unknown"}
        };
        #endif
        
        // OGA (RyzenAI-Server)
        engines["oga"] = {{"available", is_ryzenai_serve_available()}};
    }
    
    return engines;
}

std::string SystemInfo::get_llamacpp_version(const std::string& backend) {
    // Try to find version.txt in the llamacpp directory for specific backend
    // Location: {executable_dir}/{backend}/llama_server/version.txt
    
    #ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
    
    fs::path version_file = exe_dir / backend / "llama_server" / "version.txt";
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
    #else
    // For Linux, check relative to current executable
    std::string version_file = backend + "/llama_server/version.txt";
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
    #endif
    
    return "unknown";
}

bool SystemInfo::is_llamacpp_installed(const std::string& backend) {
    // Check if llama-server executable exists for the given backend
    
    #ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
    
    fs::path llama_exe = exe_dir / backend / "llama_server" / "llama-server.exe";
    return fs::exists(llama_exe);
    #else
    // For Linux, check build/bin subdirectory first, then root
    std::string build_bin_path = backend + "/llama_server/build/bin/llama-server";
    if (fs::exists(build_bin_path)) {
        return true;
    }
    std::string root_path = backend + "/llama_server/llama-server";
    return fs::exists(root_path);
    #endif
}

bool SystemInfo::check_vulkan_support() {
    // Check if Vulkan is available
    #ifdef _WIN32
    // Check for Vulkan DLL on Windows
    if (fs::exists("C:\\Windows\\System32\\vulkan-1.dll") || 
        fs::exists("C:\\Windows\\SysWOW64\\vulkan-1.dll")) {
        return true;
    }
    #else
    // Check for Vulkan libraries on Linux
    std::vector<std::string> vulkan_lib_paths = {
        "/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
        "/usr/lib/libvulkan.so.1",
        "/lib/x86_64-linux-gnu/libvulkan.so.1"
    };
    for (const auto& path : vulkan_lib_paths) {
        if (fs::exists(path)) {
            return true;
        }
    }
    #endif
    
    // Try vulkaninfo command
    #ifdef _WIN32
    FILE* pipe = _popen("vulkaninfo --summary 2>NUL", "r");
    #else
    FILE* pipe = popen("vulkaninfo --summary 2>/dev/null", "r");
    #endif
    
    if (pipe) {
        char buffer[128];
        bool has_output = (fgets(buffer, sizeof(buffer), pipe) != nullptr);
        #ifdef _WIN32
        _pclose(pipe);
        #else
        pclose(pipe);
        #endif
        return has_output;
    }
    
    return false;
}

// Helper to identify ROCm architecture from GPU name (same logic as llamacpp_server.cpp)
static std::string identify_rocm_arch_from_name(const std::string& device_name) {
    std::string device_lower = device_name;
    std::transform(device_lower.begin(), device_lower.end(), device_lower.begin(), ::tolower);
    
    if (device_lower.find("radeon") == std::string::npos) {
        return "";
    }
    
    // STX Halo iGPUs (gfx1151 architecture)
    // Radeon 8050S Graphics / Radeon 8060S Graphics
    if (device_lower.find("8050s") != std::string::npos || 
        device_lower.find("8060s") != std::string::npos) {
        return "gfx1151";
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

bool SystemInfo::check_rocm_support(const std::string& device_name) {
    // Check if device supports ROCm by attempting to identify the architecture
    // This matches the Python implementation which uses identify_rocm_arch_from_name
    std::string arch = identify_rocm_arch_from_name(device_name);
    return !arch.empty();
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
    // Use the same logic as RyzenAIServer::get_ryzenai_serve_path()
    
    #ifdef _WIN32
    std::string exe_name = "ryzenai-server.exe";
    std::string check_cmd = "where ryzenai-server.exe >nul 2>&1";
#else
    std::string exe_name = "ryzenai-server";
    std::string check_cmd = "which ryzenai-server >/dev/null 2>&1";
#endif
    
    // Check if executable exists in PATH
    if (system(check_cmd.c_str()) == 0) {
        return true;
    }
    
    // Check in common locations relative to lemonade executable
    // This uses the same path resolution as RyzenAIServer
    #ifdef _WIN32
    // Get executable path
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    fs::path exe_dir = fs::path(exe_path).parent_path();
    
    // Check relative path: from executable to ../../../ryzenai-server/build/bin/Release (source tree)
    fs::path relative_path = exe_dir / ".." / ".." / ".." / "ryzenai-server" / "build" / "bin" / "Release" / exe_name;
    if (fs::exists(relative_path)) {
        return true;
    }
    
    // Check installed location next to lemonade binary
    fs::path install_path = exe_dir / "ryzenai-server" / exe_name;
    if (fs::exists(install_path)) {
        return true;
    }
    #else
    // For Linux/macOS
    fs::path relative_path = fs::path("../../../ryzenai-server/build/bin/Release") / exe_name;
    if (fs::exists(relative_path)) {
        return true;
    }
    
    // Check installed location
    fs::path install_path = fs::path("ryzenai-server") / exe_name;
    if (fs::exists(install_path)) {
        return true;
    }
    #endif
    
    return false;
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
        
        // Detect inference engines for CPU
        cpu.inference_engines = detect_inference_engines("cpu", cpu.name);
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
                
                // Detect inference engines
                gpu.inference_engines = detect_inference_engines("nvidia_dgpu", name);
                
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
    // Check if the processor is a supported Ryzen AI processor (300-series)
    // This matches the logic in Python's check_ryzen_ai_processor() function
    
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
    
    // Convert to lowercase for case-insensitive matching
    std::string processor_lower = processor_name;
    std::transform(processor_lower.begin(), processor_lower.end(), processor_lower.begin(), ::tolower);
    
    // Check for Ryzen AI 300-series pattern: "ryzen ai" followed by a 3-digit number starting with 3
    // Pattern: ryzen ai.*\b3\d{2}\b
    std::regex pattern(R"(ryzen ai.*\b[34]\d{2}\b)", std::regex::icase);
    
    return std::regex_search(processor_lower, pattern);
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
        
        // Detect inference engines
        npu.inference_engines = detect_inference_engines("npu", "AMD NPU");
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
                
                // Detect inference engines
                std::string device_type = is_integrated ? "amd_igpu" : "amd_dgpu";
                gpu.inference_engines = detect_inference_engines(device_type, name);
                
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
    
    // Detect inference engines
    cpu.inference_engines = detect_inference_engines("cpu", cpu.name);
    
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
                
                // Detect inference engines
                gpu.inference_engines = detect_inference_engines("nvidia_dgpu", name);
                
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
    
    // Parse AMD GPUs
    for (const auto& line : lspci_lines) {
        if (line.find("AMD") != std::string::npos || line.find("ATI") != std::string::npos) {
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
            
            // Classify as discrete or integrated using keywords
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            
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
                
                // Get VRAM for discrete GPUs
                if (is_discrete) {
                    // Extract PCI ID from lspci line (first field)
                    std::string pci_id = line.substr(0, line.find(" "));
                    
                    double vram = get_amd_vram_rocm_smi();
                    if (vram == 0.0) {
                        vram = get_amd_vram_sysfs(pci_id);
                    }
                    
                    if (vram > 0.0) {
                        gpu.vram_gb = vram;
                    }
                }
                
                // Detect inference engines
                std::string device_type = is_integrated ? "amd_igpu" : "amd_dgpu";
                gpu.inference_engines = detect_inference_engines(device_type, name);
                
                gpus.push_back(gpu);
            }
        }
    }
    
    if (gpus.empty()) {
        GPUInfo gpu;
        gpu.available = false;
        gpu.error = "No AMD " + gpu_type + " GPU found";
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

double LinuxSystemInfo::get_amd_vram_rocm_smi() {
    FILE* pipe = popen("rocm-smi --showmeminfo vram --csv 2>/dev/null", "r");
    if (!pipe) {
        return 0.0;
    }
    
    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    
    // Parse CSV output for VRAM
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("Total VRAM") != std::string::npos || 
            line.find("vram") != std::string::npos) {
            // Extract numbers
            std::regex num_regex(R"(\d+)");
            std::smatch match;
            if (std::regex_search(line, match, num_regex)) {
                try {
                    double vram_value = std::stod(match[0].str());
                    // Assume MB if large value, GB if small
                    if (vram_value > 100) {
                        return std::round(vram_value / 1024.0 * 10.0) / 10.0;
                    } else {
                        return vram_value;
                    }
                } catch (...) {
                    return 0.0;
                }
            }
        }
    }
    
    return 0.0;
}

double LinuxSystemInfo::get_amd_vram_sysfs(const std::string& pci_id) {
    // Try device-specific path first
    std::string vram_path = "/sys/bus/pci/devices/" + pci_id + "/mem_info_vram_total";
    std::ifstream file(vram_path);
    
    if (!file.is_open()) {
        // Try wildcard path
        FILE* pipe = popen("cat /sys/class/drm/card*/device/mem_info_vram_total 2>/dev/null | head -1", "r");
        if (pipe) {
            char buffer[128];
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                pclose(pipe);
                try {
                    uint64_t vram_bytes = std::stoull(buffer);
                    return std::round(vram_bytes / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0;
                } catch (...) {
                    return 0.0;
                }
            }
            pclose(pipe);
        }
        return 0.0;
    }
    
    std::string vram_str;
    std::getline(file, vram_str);
    file.close();
    
    try {
        uint64_t vram_bytes = std::stoull(vram_str);
        return std::round(vram_bytes / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0;
    } catch (...) {
        return 0.0;
    }
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
    
    // Get kernel version using uname
    FILE* pipe = popen("uname -r 2>/dev/null", "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string kernel = buffer;
            // Remove newline
            if (!kernel.empty() && kernel.back() == '\n') {
                kernel.pop_back();
            }
            result += "-" + kernel;
        }
        pclose(pipe);
    }
    
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
    FILE* pipe = popen("free -m 2>/dev/null", "r");
    if (!pipe) {
        return "ERROR - Failed to execute free command";
    }
    
    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    
    // Parse output - second line contains memory info
    std::istringstream iss(output);
    std::string line;
    int line_count = 0;
    while (std::getline(iss, line)) {
        line_count++;
        if (line_count == 2) {  // Second line has memory data
            std::istringstream line_stream(line);
            std::string token;
            int token_count = 0;
            while (line_stream >> token) {
                token_count++;
                if (token_count == 2) {  // Second token is total memory in MB
                    try {
                        int mem_mb = std::stoi(token);
                        double mem_gb = std::round(mem_mb / 1024.0 * 100.0) / 100.0;
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(2) << mem_gb << " GB";
                        return oss.str();
                    } catch (...) {
                        return "ERROR - Failed to parse memory info";
                    }
                }
            }
        }
    }
    
    return "ERROR - Memory information not found";
}

#endif // __linux__

// ============================================================================
// macOS implementation (stubs)
// ============================================================================

#ifdef __APPLE__

CPUInfo MacOSSystemInfo::get_cpu_device() {
    CPUInfo cpu;
    cpu.available = false;
    cpu.error = "macOS CPU detection not implemented yet";
    return cpu;
}

GPUInfo MacOSSystemInfo::get_amd_igpu_device() {
    GPUInfo gpu;
    gpu.available = false;
    gpu.error = "macOS AMD iGPU detection not implemented yet";
    return gpu;
}

std::vector<GPUInfo> MacOSSystemInfo::get_amd_dgpu_devices() {
    return {};
}

std::vector<GPUInfo> MacOSSystemInfo::get_nvidia_dgpu_devices() {
    return {};
}

NPUInfo MacOSSystemInfo::get_npu_device() {
    NPUInfo npu;
    npu.available = false;
    npu.error = "macOS NPU detection not implemented yet";
    return npu;
}

#endif // __APPLE__

// ============================================================================
// Cache implementation
// ============================================================================

SystemInfoCache::SystemInfoCache() {
    cache_file_path_ = get_cache_dir() + "/hardware_info.json";
}

std::string SystemInfoCache::get_cache_dir() const {
    const char* cache_dir_env = std::getenv("LEMONADE_CACHE_DIR");
    if (cache_dir_env) {
        return std::string(cache_dir_env);
    }
    
    #ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.cache\\lemonade";
    }
    #else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/lemonade";
    }
    #endif
    
    return ".cache/lemonade";
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

json SystemInfoCache::get_system_info_with_cache(bool verbose) {
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
            std::cout << "[Server] Using cached hardware info" << std::endl;
            system_info = cached_data;
        } else {
            // Provide friendly message about why we're detecting hardware
            if (cache_exists) {
                std::cout << "[Server] Collecting system info (Lemonade was updated)" << std::endl;
            } else {
                std::cout << "[Server] Collecting system info" << std::endl;
            }
            
            // Get system info (OS, Processor, Memory, etc.)
            try {
                system_info = sys_info->get_system_info_dict();
            } catch (...) {
                system_info["OS Version"] = "Unknown";
            }
            
            // Get device information - handles its own exceptions internally
            system_info["devices"] = sys_info->get_device_dict();
            
            // Save to cache (without inference_engines which are always fresh)
            try {
                cache.save_hardware_info(system_info);
            } catch (...) {
                // Cache save failed - not critical, continue
            }
        }
        
        // Add inference engines (always fresh, never cached)
        if (system_info.contains("devices")) {
            auto& devices = system_info["devices"];
            
            // Helper to safely add inference engines
            auto add_engines = [&](const std::string& key, const std::string& device_type) {
                if (devices.contains(key) && devices[key].contains("name")) {
                    std::string name = devices[key]["name"];
                    devices[key]["inference_engines"] = sys_info->detect_inference_engines(device_type, name);
                }
            };
            
            add_engines("cpu", "cpu");
            add_engines("amd_igpu", "amd_igpu");
            add_engines("npu", "npu");
            
            // Handle GPU arrays
            for (const auto& gpu_type : {"amd_dgpu", "nvidia_dgpu"}) {
                if (devices.contains(gpu_type) && devices[gpu_type].is_array()) {
                    for (auto& gpu : devices[gpu_type]) {
                        if (gpu.contains("name") && !gpu["name"].get<std::string>().empty()) {
                            gpu["inference_engines"] = sys_info->detect_inference_engines(gpu_type, gpu["name"]);
                        }
                    }
                }
            }
        }
        
        // Filter for non-verbose mode
        if (!verbose) {
            std::vector<std::string> essential_keys = {"OS Version", "Processor", "Physical Memory", "devices"};
            json filtered_info;
            for (const auto& key : essential_keys) {
                if (system_info.contains(key)) {
                    filtered_info[key] = system_info[key];
                }
            }
            system_info = filtered_info;
        } else {
            system_info["Python Packages"] = SystemInfo::get_python_packages();
        }
        
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
    
    return system_info;
}


} // namespace lemon

