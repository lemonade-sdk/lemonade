#pragma once

#include <cstdint>
#include <lemon/system_metrics_platform.h>
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

using NvidiaMetrics = SystemGpuMetrics;

namespace nvidia_metrics_detail {

// Keep the minimal NVML ABI local so telemetry does not add a build-time
// CUDA/NVML dependency.
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr unsigned int NVML_DEVICE_NAME_BUFFER_SIZE = 96;
constexpr unsigned int NVML_DEVICE_UUID_BUFFER_SIZE = 96;
constexpr unsigned int NVML_DRIVER_VERSION_BUFFER_SIZE = 96;

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

    ~NvmlLibrary() {
        close_library();
    }

    NvmlLibrary(const NvmlLibrary&) = delete;
    NvmlLibrary& operator=(const NvmlLibrary&) = delete;

    std::vector<NvidiaNvmlDevice> query_devices(
        bool include_utilization = true) const {
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

            if (include_utilization && get_utilization_) {
                nvmlUtilization_t utilization{};
                if (get_utilization_(device, &utilization) == NVML_SUCCESS) {
                    info.gpu_percent = static_cast<double>(utilization.gpu);
                }
            }

            if (get_memory_) {
                nvmlMemory_t memory{};
                if (get_memory_(device, &memory) == NVML_SUCCESS) {
                    info.vram_total_gb =
                        static_cast<double>(memory.total) / BYTES_PER_GIB;
                    info.vram_used_gb =
                        static_cast<double>(memory.used) / BYTES_PER_GIB;
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

    void close_library() {
        if (library_) {
            FreeLibrary(library_);
            library_ = nullptr;
        }
    }
#elif defined(__linux__)
    static LibraryHandle open_library() {
        const char* candidates[] = {
            "libnvidia-ml.so.1",
            "/var/lib/snapd/lib/gl/libnvidia-ml.so.1",
#if defined(__x86_64__)
            "/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
            "/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
#elif defined(__aarch64__)
            "/usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1",
            "/lib/aarch64-linux-gnu/libnvidia-ml.so.1",
#endif
            "/usr/lib64/libnvidia-ml.so.1",
            "/lib64/libnvidia-ml.so.1",
        };
        for (const char* candidate : candidates) {
            if (void* library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL)) {
                return library;
            }
        }
        return nullptr;
    }

    RawSymbol symbol(const char* name) const {
        return library_ ? dlsym(library_, name) : nullptr;
    }

    void close_library() {
        if (library_) {
            dlclose(library_);
            library_ = nullptr;
        }
    }
#else
    static LibraryHandle open_library() {
        return nullptr;
    }

    RawSymbol symbol(const char*) const {
        return nullptr;
    }

    void close_library() {
        library_ = nullptr;
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

} // namespace nvidia_metrics_detail

inline std::vector<NvidiaNvmlDevice> query_nvidia_nvml_devices(
    bool include_utilization = true) {
    nvidia_metrics_detail::NvmlLibrary library;
    return library.query_devices(include_utilization);
}

inline NvidiaMetrics aggregate_nvidia_metrics(
    const std::vector<NvidiaNvmlDevice>& devices) {
    NvidiaMetrics result;

    // Independent device pools are not additive for this scalar API. Keep the
    // largest observed value for each resource, independently of device activity.
    for (const auto& device : devices) {
        system_metrics_detail::merge_max(
            result, {device.gpu_percent, device.vram_used_gb});
    }
    return result;
}

inline NvidiaMetrics query_nvidia_metrics() {
    return aggregate_nvidia_metrics(query_nvidia_nvml_devices());
}

} // namespace lemon
