#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace lemon {

struct NvidiaNvmlDevice {
    int index = -1;
    std::string uuid;
    std::string name;
    std::string compute_cap;
    std::string driver_version;
    double gpu_percent = -1.0;
    double vram_total_gb = -1.0;
    double vram_used_gb = -1.0;
};

struct NvidiaMetrics {
    double gpu_percent = -1.0;
    double vram_used_gb = -1.0;
};

namespace nvidia_metrics_detail {

// Keep the minimal NVML ABI local so telemetry does not add a build-time
// CUDA/NVML dependency.
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_DEVICE_NAME_BUFFER_SIZE = 96;
constexpr unsigned int NVML_DEVICE_UUID_BUFFER_SIZE = 96;
constexpr unsigned int NVML_DRIVER_VERSION_BUFFER_SIZE = 96;
constexpr double BYTES_PER_GIB = 1024.0 * 1024.0 * 1024.0;

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

class NvmlLibrary {
public:
    NvmlLibrary() {
        load();
    }

    std::vector<NvidiaNvmlDevice> query_devices() const {
        std::vector<NvidiaNvmlDevice> devices;
        if (!ready_ || init_() != NVML_SUCCESS) {
            return devices;
        }

        // NVML documents init/shutdown as reference-counted and the library as
        // thread-safe. Each query owns one balanced init reference, so there is
        // no process-lifetime initialized singleton and no atexit shutdown race.
        struct ShutdownGuard {
            ShutdownFn shutdown;
            ~ShutdownGuard() {
                if (shutdown) {
                    shutdown();
                }
            }
        } shutdown_guard{shutdown_};

        unsigned int count = 0;
        if (get_count_(&count) != NVML_SUCCESS) {
            return devices;
        }

        char driver_buffer[NVML_DRIVER_VERSION_BUFFER_SIZE] = {};
        std::string driver_version;
        if (get_driver_ &&
            get_driver_(driver_buffer, sizeof(driver_buffer)) == NVML_SUCCESS) {
            driver_version = driver_buffer;
        }

        devices.reserve(count);
        for (unsigned int index = 0; index < count; ++index) {
            nvmlDevice_t device = nullptr;
            if (get_handle_(index, &device) != NVML_SUCCESS || !device) {
                continue;
            }

            NvidiaNvmlDevice info;
            info.index = static_cast<int>(index);
            info.driver_version = driver_version;

            if (get_name_) {
                char name_buffer[NVML_DEVICE_NAME_BUFFER_SIZE] = {};
                if (get_name_(device, name_buffer, sizeof(name_buffer)) == NVML_SUCCESS) {
                    info.name = name_buffer;
                }
            }

            if (get_uuid_) {
                char uuid_buffer[NVML_DEVICE_UUID_BUFFER_SIZE] = {};
                if (get_uuid_(device, uuid_buffer, sizeof(uuid_buffer)) == NVML_SUCCESS) {
                    info.uuid = uuid_buffer;
                }
            }

            if (get_compute_cap_) {
                int major = 0;
                int minor = 0;
                if (get_compute_cap_(device, &major, &minor) == NVML_SUCCESS) {
                    info.compute_cap = std::to_string(major) + "." + std::to_string(minor);
                }
            }

            if (get_utilization_) {
                nvmlUtilization_t utilization{};
                if (get_utilization_(device, &utilization) == NVML_SUCCESS) {
                    info.gpu_percent = static_cast<double>(utilization.gpu);
                }
            }

            if (get_memory_) {
                nvmlMemory_t memory{};
                if (get_memory_(device, &memory) == NVML_SUCCESS) {
                    info.vram_total_gb = static_cast<double>(memory.total) / BYTES_PER_GIB;
                    info.vram_used_gb = static_cast<double>(memory.used) / BYTES_PER_GIB;
                }
            }

            devices.push_back(info);
        }

        return devices;
    }

private:
    using InitFn = nvmlReturn_t (*)();
    using ShutdownFn = nvmlReturn_t (*)();
    using GetCountFn = nvmlReturn_t (*)(unsigned int*);
    using GetHandleFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
    using GetNameFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    using GetUuidFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
    using GetComputeCapFn = nvmlReturn_t (*)(nvmlDevice_t, int*, int*);
    using GetDriverFn = nvmlReturn_t (*)(char*, unsigned int);
    using GetUtilizationFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
    using GetMemoryFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);

#ifdef _WIN32
    using LibraryHandle = HMODULE;
    using RawSymbol = FARPROC;
#else
    using LibraryHandle = void*;
    using RawSymbol = void*;
#endif

    LibraryHandle library_ = nullptr;
    InitFn init_ = nullptr;
    ShutdownFn shutdown_ = nullptr;
    GetCountFn get_count_ = nullptr;
    GetHandleFn get_handle_ = nullptr;
    GetNameFn get_name_ = nullptr;
    GetUuidFn get_uuid_ = nullptr;
    GetComputeCapFn get_compute_cap_ = nullptr;
    GetDriverFn get_driver_ = nullptr;
    GetUtilizationFn get_utilization_ = nullptr;
    GetMemoryFn get_memory_ = nullptr;
    bool ready_ = false;

#ifdef _WIN32
    static LibraryHandle open_library() {
        if (HMODULE library = LoadLibraryA("nvml.dll")) {
            return library;
        }

        for (const char* env_name : {"ProgramW6432", "ProgramFiles"}) {
            char program_files[MAX_PATH] = {};
            const DWORD length = GetEnvironmentVariableA(
                env_name, program_files, static_cast<DWORD>(sizeof(program_files)));
            if (length == 0 || length >= sizeof(program_files)) {
                continue;
            }

            const std::string path = std::string(program_files)
                + "\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            if (HMODULE library = LoadLibraryA(path.c_str())) {
                return library;
            }
        }
        return nullptr;
    }

    RawSymbol symbol(const char* name) const {
        return library_ ? GetProcAddress(library_, name) : nullptr;
    }
#elif defined(__linux__)
    static LibraryHandle open_library() {
        for (const char* name : {"libnvidia-ml.so.1", "libnvidia-ml.so"}) {
            if (void* library = dlopen(name, RTLD_NOW | RTLD_LOCAL)) {
                return library;
            }
        }
        return nullptr;
    }

    RawSymbol symbol(const char* name) const {
        return library_ ? dlsym(library_, name) : nullptr;
    }
#else
    static LibraryHandle open_library() {
        return nullptr;
    }

    RawSymbol symbol(const char*) const {
        return nullptr;
    }
#endif

    template <typename T>
    T load_symbol(const char* name) const {
        return reinterpret_cast<T>(symbol(name));
    }

    template <typename T>
    T load_symbol_with_fallback(const char* preferred, const char* fallback) const {
        T result = load_symbol<T>(preferred);
        return result ? result : load_symbol<T>(fallback);
    }

    void load() {
        library_ = open_library();
        if (!library_) {
            return;
        }

        init_ = load_symbol_with_fallback<InitFn>("nvmlInit_v2", "nvmlInit");
        shutdown_ = load_symbol<ShutdownFn>("nvmlShutdown");
        get_count_ = load_symbol_with_fallback<GetCountFn>(
            "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
        get_handle_ = load_symbol_with_fallback<GetHandleFn>(
            "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
        get_name_ = load_symbol<GetNameFn>("nvmlDeviceGetName");
        get_uuid_ = load_symbol<GetUuidFn>("nvmlDeviceGetUUID");
        get_compute_cap_ = load_symbol<GetComputeCapFn>("nvmlDeviceGetCudaComputeCapability");
        get_driver_ = load_symbol<GetDriverFn>("nvmlSystemGetDriverVersion");
        get_utilization_ = load_symbol<GetUtilizationFn>("nvmlDeviceGetUtilizationRates");
        get_memory_ = load_symbol<GetMemoryFn>("nvmlDeviceGetMemoryInfo");

        ready_ = init_ && shutdown_ && get_count_ && get_handle_ &&
            (get_name_ || get_utilization_ || get_memory_);
    }
};

inline NvmlLibrary& nvml_library() {
    // Keep only the dlopen/LoadLibrary handle mapped for process lifetime. NVML
    // itself is initialized only inside query_devices() and shut down before the
    // query returns. Intentionally avoiding static destruction also prevents a
    // late dlclose from racing another thread during process teardown.
    static NvmlLibrary* library = new NvmlLibrary();
    return *library;
}

} // namespace nvidia_metrics_detail

inline std::vector<NvidiaNvmlDevice> query_nvidia_nvml_devices() {
    return nvidia_metrics_detail::nvml_library().query_devices();
}

inline NvidiaMetrics aggregate_nvidia_metrics(
    const std::vector<NvidiaNvmlDevice>& devices) {
    NvidiaMetrics result;
    double total_used_gb = 0.0;
    bool have_memory = false;

    for (const auto& device : devices) {
        if (device.gpu_percent >= 0.0 &&
            (result.gpu_percent < 0.0 || device.gpu_percent > result.gpu_percent)) {
            result.gpu_percent = device.gpu_percent;
        }
        if (device.vram_used_gb >= 0.0) {
            total_used_gb += device.vram_used_gb;
            have_memory = true;
        }
    }

    if (have_memory) {
        result.vram_used_gb = total_used_gb;
    }
    return result;
}

inline NvidiaMetrics query_nvidia_metrics() {
    struct Cache {
        std::mutex mutex;
        bool valid = false;
        std::chrono::steady_clock::time_point sampled_at{};
        NvidiaMetrics metrics{};
    };
    static Cache cache;

    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (cache.valid && now - cache.sampled_at < std::chrono::milliseconds(250)) {
        return cache.metrics;
    }

    cache.metrics = aggregate_nvidia_metrics(query_nvidia_nvml_devices());
    cache.sampled_at = now;
    cache.valid = true;
    return cache.metrics;
}

} // namespace lemon
