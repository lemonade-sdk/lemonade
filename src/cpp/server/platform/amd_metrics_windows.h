#pragma once

// AMD GPU telemetry for Windows system-stats, via DXGI, DXCore and PDH.
// Libraries are loaded dynamically so the build needs no vendor SDK.

#ifdef _WIN32

#include <lemon/system_metrics_platform.h>

#include <windows.h>
#include <dxgi.h>
#include <dxcore.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lemon {

namespace amd_metrics_detail {

constexpr unsigned int kAmdVendorId = 0x1002;
using LibraryHandle = HMODULE;

inline std::string narrow(const wchar_t* wide) {
    if (!wide) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Canonical adapter key. DXGI LUIDs and PDH "\GPU Engine(..._luid_H_L_...)"
// instance names both route through this, so the two always agree byte-for-byte.
inline std::string luid_token(unsigned long high, unsigned long low) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%08lX_0x%08lX", high, low);
    return buf;
}

inline std::string luid_token(const LUID& luid) {
    return luid_token(static_cast<unsigned long>(luid.HighPart),
                      static_cast<unsigned long>(luid.LowPart));
}

struct AmdAdapter {
    std::string id;          // "amd:<luid>"
    std::string name;
    std::string luid;        // matches PDH instance-name LUIDs
    double memory_total_gb = -1.0;
    std::optional<bool> integrated;  // nullopt when DXCore can't classify it
};

// ---- DXCore: integrated vs discrete classification --------------------------

class DxCoreLibrary {
public:
    DxCoreLibrary() {
        library_ = LoadLibraryExA("dxcore.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library_) {
            create_ = reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(
                GetProcAddress(library_, "DXCoreCreateAdapterFactory")));
        }
        if (create_) {
            create_(IID_PPV_ARGS(&factory_));
        }
    }

    ~DxCoreLibrary() {
        if (factory_) factory_->Release();
        if (library_) FreeLibrary(library_);
    }

    DxCoreLibrary(const DxCoreLibrary&) = delete;
    DxCoreLibrary& operator=(const DxCoreLibrary&) = delete;

    std::optional<bool> is_integrated(const LUID& luid) const {
        if (!factory_) return std::nullopt;
        IDXCoreAdapter* adapter = nullptr;
        if (FAILED(factory_->GetAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            !adapter) {
            return std::nullopt;
        }
        std::optional<bool> result;
        bool value = false;
        if (adapter->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated) &&
            SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated,
                                           &value))) {
            result = value;
        }
        adapter->Release();
        return result;
    }

private:
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    LibraryHandle library_ = nullptr;
    CreateFactoryFn create_ = nullptr;
    IDXCoreAdapterFactory* factory_ = nullptr;
};

// ---- DXGI: adapter identity and memory capacity -----------------------------

class DxgiLibrary {
public:
    DxgiLibrary() {
        char system_dir[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryA(system_dir, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const std::string path = std::string(system_dir) + "\\dxgi.dll";
            library_ = LoadLibraryExA(path.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (library_) {
            create_ = reinterpret_cast<CreateFactory1Fn>(
                reinterpret_cast<void*>(
                    GetProcAddress(library_, "CreateDXGIFactory1")));
        }
    }

    ~DxgiLibrary() {
        if (library_) FreeLibrary(library_);
    }

    DxgiLibrary(const DxgiLibrary&) = delete;
    DxgiLibrary& operator=(const DxgiLibrary&) = delete;

    bool ready() const { return create_ != nullptr; }

    std::vector<AmdAdapter> query_adapters(const DxCoreLibrary& dxcore) const {
        std::vector<AmdAdapter> adapters;
        if (!create_) return adapters;

        IDXGIFactory1* factory = nullptr;
        if (FAILED(create_(__uuidof(IDXGIFactory1),
                           reinterpret_cast<void**>(&factory))) ||
            !factory) {
            return adapters;
        }

        IDXGIAdapter1* adapter1 = nullptr;
        for (UINT i = 0;
             factory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
            if (!adapter1) continue;

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter1->GetDesc1(&desc)) &&
                desc.VendorId == kAmdVendorId &&
                (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                adapters.push_back(
                    make_adapter(desc, dxcore.is_integrated(desc.AdapterLuid)));
            }
            adapter1->Release();
            adapter1 = nullptr;
        }
        factory->Release();
        return adapters;
    }

private:
    using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);

    static AmdAdapter make_adapter(const DXGI_ADAPTER_DESC1& desc,
                                   std::optional<bool> integrated) {
        AmdAdapter entry;
        entry.luid = luid_token(desc.AdapterLuid);
        entry.id = "amd:" + entry.luid;
        entry.name = narrow(desc.Description);
        entry.integrated = integrated;

        if (!integrated) return entry;  // unclassified: no reliable capacity
        // A UMA adapter's capacity is both pools, matching its combined usage.
        const uint64_t total = *integrated
            ? desc.DedicatedVideoMemory + desc.SharedSystemMemory
            : desc.DedicatedVideoMemory;
        if (total > 0) entry.memory_total_gb = total / BYTES_PER_GIB;
        return entry;
    }

    LibraryHandle library_ = nullptr;
    CreateFactory1Fn create_ = nullptr;
};

// ---- PDH: live utilization and memory usage ---------------------------------

struct AmdLiveMetrics {
    std::map<std::string, double> utilization;      // luid -> busiest-engine %
    std::map<std::string, double> dedicated_used;   // luid -> dedicated bytes
    std::map<std::string, double> shared_used;      // luid -> shared bytes
};

class PdhGpuCounters {
public:
    PdhGpuCounters() {
        library_ = LoadLibraryExA("pdh.dll", nullptr,
                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) return;
        open_ = sym<OpenQueryFn>("PdhOpenQueryA");
        add_ = sym<AddCounterFn>("PdhAddEnglishCounterA");
        collect_ = sym<CollectFn>("PdhCollectQueryData");
        format_array_ = sym<FormatArrayFn>("PdhGetFormattedCounterArrayA");
        close_ = sym<CloseQueryFn>("PdhCloseQuery");
        if (!open_ || !add_ || !collect_ || !format_array_ || !close_ ||
            open_(nullptr, 0, &query_) != ERROR_SUCCESS) {
            return;
        }
        const bool util_ok =
            add_(query_, "\\GPU Engine(*)\\Utilization Percentage", 0,
                 &util_counter_) == ERROR_SUCCESS;
        const bool ded_ok =
            add_(query_, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0,
                 &dedicated_counter_) == ERROR_SUCCESS;
        const bool shr_ok =
            add_(query_, "\\GPU Adapter Memory(*)\\Shared Usage", 0,
                 &shared_counter_) == ERROR_SUCCESS;
        if (util_ok || ded_ok || shr_ok) {
            collect_(query_);  // prime: the first collect has no delta to format
            ready_ = true;
        }
    }

    ~PdhGpuCounters() {
        if (query_ && close_) close_(query_);
        if (library_) FreeLibrary(library_);
    }

    PdhGpuCounters(const PdhGpuCounters&) = delete;
    PdhGpuCounters& operator=(const PdhGpuCounters&) = delete;

    bool ready() const { return ready_; }

    AmdLiveMetrics sample() {
        AmdLiveMetrics out;
        if (!ready_ || collect_(query_) != ERROR_SUCCESS) return out;

        // GPU Engine instances are per-process-per-engine. Sum each engine's
        // load across processes, then take the busiest engine, so concurrent
        // processes on one engine are not undercounted.
        std::map<std::string, double> per_engine;  // "luid|eng" -> summed %
        for_each_item(util_counter_, [&](const std::string& name, double v) {
            const std::string luid = extract_luid(name);
            if (luid.empty()) return;
            per_engine[luid + "|" + extract_engine(name)] += v;
        });
        for (const auto& [key, load] : per_engine) {
            const std::string luid = key.substr(0, key.find('|'));
            const double clamped = load > 100.0 ? 100.0 : load;
            double& slot = out.utilization[luid];
            if (clamped > slot) slot = clamped;
        }

        accumulate_by_luid(dedicated_counter_, out.dedicated_used);
        accumulate_by_luid(shared_counter_, out.shared_used);
        return out;
    }

private:
    using OpenQueryFn = PDH_STATUS(WINAPI*)(LPCSTR, DWORD_PTR, PDH_HQUERY*);
    using AddCounterFn =
        PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER*);
    using CollectFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using FormatArrayFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD,
                                              LPDWORD,
                                              PDH_FMT_COUNTERVALUE_ITEM_A*);
    using CloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);

    template <typename T>
    T sym(const char* name) const {
        return library_ ? reinterpret_cast<T>(reinterpret_cast<void*>(
                              GetProcAddress(library_, name)))
                        : nullptr;
    }

    // Instance names look like "pid_1234_luid_0x00000000_0x0002AEB3_phys_0_...".
    static std::string extract_luid(const std::string& name) {
        const size_t tag = name.find("luid_");
        if (tag == std::string::npos) return {};
        unsigned long high = 0, low = 0;
        if (std::sscanf(name.c_str() + tag, "luid_%lx_%lx", &high, &low) != 2)
            return {};
        return luid_token(high, low);
    }

    // The "phys_N_eng_M" tail identifies a physical engine on the adapter.
    static std::string extract_engine(const std::string& name) {
        const size_t phys = name.find("phys_");
        return phys == std::string::npos ? std::string() : name.substr(phys);
    }

    void accumulate_by_luid(PDH_HCOUNTER counter,
                            std::map<std::string, double>& out) {
        for_each_item(counter, [&](const std::string& name, double v) {
            const std::string luid = extract_luid(name);
            if (!luid.empty()) out[luid] += v;
        });
    }

    template <typename Fn>
    void for_each_item(PDH_HCOUNTER counter, Fn&& fn) {
        if (!counter) return;
        DWORD buffer_size = 0;
        DWORD item_count = 0;
        if (format_array_(counter, PDH_FMT_DOUBLE, &buffer_size, &item_count,
                          nullptr) != PDH_MORE_DATA || buffer_size == 0) {
            return;
        }
        std::vector<unsigned char> buffer(buffer_size);
        auto* items =
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buffer.data());
        if (format_array_(counter, PDH_FMT_DOUBLE, &buffer_size, &item_count,
                          items) != ERROR_SUCCESS) {
            return;
        }
        for (DWORD i = 0; i < item_count; ++i) {
            const auto& item = items[i];
            if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            if (item.szName && item.FmtValue.doubleValue >= 0.0)
                fn(std::string(item.szName), item.FmtValue.doubleValue);
        }
    }

    LibraryHandle library_ = nullptr;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER util_counter_ = nullptr;
    PDH_HCOUNTER dedicated_counter_ = nullptr;
    PDH_HCOUNTER shared_counter_ = nullptr;
    OpenQueryFn open_ = nullptr;
    AddCounterFn add_ = nullptr;
    CollectFn collect_ = nullptr;
    FormatArrayFn format_array_ = nullptr;
    CloseQueryFn close_ = nullptr;
    bool ready_ = false;
};

// Kept alive for the process so utilization has a delta between polls. Leaked
// intentionally: the OS reclaims the handles at exit, avoiding a static-dtor
// race against an in-flight sample().
inline PdhGpuCounters& pdh_counters() {
    static auto* instance = new PdhGpuCounters();
    return *instance;
}

inline std::mutex& pdh_mutex() {
    static auto* m = new std::mutex();
    return *m;
}

inline AmdLiveMetrics sample_live_metrics() {
    std::lock_guard<std::mutex> lock(pdh_mutex());
    auto& pdh = pdh_counters();
    return pdh.ready() ? pdh.sample() : AmdLiveMetrics{};
}

}  // namespace amd_metrics_detail

inline std::vector<GpuDeviceMetrics> query_amd_gpu_metrics() {
    using namespace amd_metrics_detail;

    DxgiLibrary dxgi;
    if (!dxgi.ready()) return {};
    DxCoreLibrary dxcore;
    const auto adapters = dxgi.query_adapters(dxcore);
    if (adapters.empty()) return {};

    const AmdLiveMetrics live = sample_live_metrics();

    std::vector<GpuDeviceMetrics> result;
    result.reserve(adapters.size());
    for (const auto& adapter : adapters) {
        const bool integrated = adapter.integrated.value_or(false);

        GpuDeviceMetrics gpu;
        gpu.id = adapter.id;
        gpu.vendor = "amd";
        gpu.name = adapter.name.empty() ? "AMD GPU" : adapter.name;
        gpu.memory_type = !adapter.integrated ? "unknown"
                        : integrated          ? "shared"
                                              : "dedicated";
        gpu.memory_total_gb = adapter.memory_total_gb;

        if (const auto it = live.utilization.find(adapter.luid);
            it != live.utilization.end()) {
            gpu.gpu_percent = it->second;
        }

        // Counter selection needs a known memory pool. A UMA adapter spans both
        // pools, so report usage only when both readings are present.
        if (adapter.integrated) {
            const auto ded = live.dedicated_used.find(adapter.luid);
            const auto shr = live.shared_used.find(adapter.luid);
            const bool has_ded = ded != live.dedicated_used.end();
            const bool has_shr = shr != live.shared_used.end();
            if (integrated && has_ded && has_shr) {
                gpu.memory_used_gb = (ded->second + shr->second) / BYTES_PER_GIB;
            } else if (!integrated && has_ded) {
                gpu.memory_used_gb = ded->second / BYTES_PER_GIB;
            }
        }

        result.push_back(std::move(gpu));
    }
    return result;
}

}  // namespace lemon

#endif  // _WIN32
