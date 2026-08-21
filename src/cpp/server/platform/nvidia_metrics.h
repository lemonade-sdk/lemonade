#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace lemon {

struct NvidiaMetrics {
    double gpu_percent = -1.0;
    double vram_used_gb = -1.0;
};

namespace nvidia_metrics_detail {

// Keep the NVML ABI local so driver telemetry does not add a build-time CUDA/NVML dependency.
using nvmlReturn_t = int;
using nvmlDevice_t = struct nvmlDevice_st*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

class NvmlProbe {
public:
    NvmlProbe() {
        load();
    }

    ~NvmlProbe() {
        if (initialized_ && shutdown_) {
            shutdown_();
        }
        unload_library();
    }

    NvidiaMetrics sample() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (has_cached_sample_ && now - cached_at_ < std::chrono::milliseconds(250)) {
            return cached_sample_;
        }

        cached_sample_ = sample_uncached();
        cached_at_ = now;
        has_cached_sample_ = true;
        return cached_sample_;
    }

private:
    using InitFn = nvmlReturn_t (*)();
    using ShutdownFn = nvmlReturn_t (*)();
    using GetCountFn = nvmlReturn_t (*)(unsigned int*);
    using GetHandleFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
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
    GetUtilizationFn get_utilization_ = nullptr;
    GetMemoryFn get_memory_ = nullptr;
    bool initialized_ = false;
    bool has_cached_sample_ = false;
    NvidiaMetrics cached_sample_{};
    std::chrono::steady_clock::time_point cached_at_{};
    std::mutex mutex_;

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

    void unload_library() {
        if (library_) {
            FreeLibrary(library_);
            library_ = nullptr;
        }
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

    void unload_library() {
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

    void unload_library() {}
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
        get_utilization_ = load_symbol<GetUtilizationFn>("nvmlDeviceGetUtilizationRates");
        get_memory_ = load_symbol<GetMemoryFn>("nvmlDeviceGetMemoryInfo");

        if (!init_ || !shutdown_ || !get_count_ || !get_handle_ ||
            (!get_utilization_ && !get_memory_)) {
            unload_library();
            return;
        }

        initialized_ = init_() == NVML_SUCCESS;
        if (!initialized_) {
            unload_library();
        }
    }

    NvidiaMetrics sample_uncached() const {
        NvidiaMetrics best;
        if (!initialized_) {
            return best;
        }

        unsigned int count = 0;
        if (get_count_(&count) != NVML_SUCCESS || count == 0) {
            return best;
        }

        bool found = false;
        double best_utilization = -1.0;
        unsigned long long best_used_bytes = 0;
        constexpr double bytes_per_gib = 1024.0 * 1024.0 * 1024.0;

        for (unsigned int index = 0; index < count; ++index) {
            nvmlDevice_t device = nullptr;
            if (get_handle_(index, &device) != NVML_SUCCESS || !device) {
                continue;
            }

            nvmlUtilization_t utilization{};
            nvmlMemory_t memory{};
            const bool has_utilization = get_utilization_
                && get_utilization_(device, &utilization) == NVML_SUCCESS;
            const bool has_memory = get_memory_
                && get_memory_(device, &memory) == NVML_SUCCESS;
            if (!has_utilization && !has_memory) {
                continue;
            }

            const double gpu_percent = has_utilization
                ? static_cast<double>(utilization.gpu)
                : -1.0;
            const unsigned long long used_bytes = has_memory ? memory.used : 0;
            const bool better = !found
                || gpu_percent > best_utilization
                || (gpu_percent == best_utilization && used_bytes > best_used_bytes);
            if (!better) {
                continue;
            }

            found = true;
            best_utilization = gpu_percent;
            best_used_bytes = used_bytes;
            best.gpu_percent = gpu_percent;
            best.vram_used_gb = has_memory
                ? static_cast<double>(memory.used) / bytes_per_gib
                : -1.0;
        }

        return best;
    }
};

} // namespace nvidia_metrics_detail

inline NvidiaMetrics query_nvidia_metrics() {
    static nvidia_metrics_detail::NvmlProbe probe;
    return probe.sample();
}

} // namespace lemon
