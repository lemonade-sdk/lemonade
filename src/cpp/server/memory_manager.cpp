#include "lemon/memory_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace {

constexpr uint64_t KiB = 1024ULL;
constexpr uint64_t MiB = 1024ULL * KiB;
constexpr uint64_t GiB = 1024ULL * MiB;

uint64_t saturating_add(uint64_t a, uint64_t b) {
    if (std::numeric_limits<uint64_t>::max() - a < b) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
}

uint64_t saturating_mul(uint64_t a, uint64_t b) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a * b;
}

uint64_t file_size_bytes(const std::string& path) {
    if (path.empty()) {
        return 0;
    }

    std::error_code ec;
    uint64_t size = static_cast<uint64_t>(fs::file_size(utils::path_from_utf8(path), ec));
    return (!ec && size > 0) ? size : 0;
}

uint64_t read_u64(std::ifstream& in) {
    uint64_t v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("Unexpected end of GGUF metadata");
    return v;
}

uint32_t read_u32(std::ifstream& in) {
    uint32_t v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("Unexpected end of GGUF metadata");
    return v;
}

std::string read_gguf_string(std::ifstream& in) {
    uint64_t len = read_u64(in);
    if (len > (1ULL << 30)) {
        throw std::runtime_error("Invalid GGUF string length");
    }
    std::string s(static_cast<size_t>(len), '\0');
    if (len > 0) {
        in.read(s.data(), static_cast<std::streamsize>(len));
        if (!in) throw std::runtime_error("Unexpected end of GGUF metadata");
    }
    return s;
}

void skip_bytes(std::ifstream& in, uint64_t n) {
    if (n > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("GGUF metadata value is too large to skip");
    }
    in.seekg(static_cast<std::streamoff>(n), std::ios::cur);
    if (!in) throw std::runtime_error("Unexpected end of GGUF metadata");
}

void skip_gguf_value(std::ifstream& in, uint32_t type);

uint64_t read_scalar_or_skip(std::ifstream& in, uint32_t type, bool& ok) {
    ok = true;
    switch (type) {
    case 0: { uint8_t v = 0; in.read(reinterpret_cast<char*>(&v), 1); return v; }
    case 2: { uint16_t v = 0; in.read(reinterpret_cast<char*>(&v), 2); return v; }
    case 4: return read_u32(in);
    case 10: return read_u64(in);
    case 1: { int8_t v = 0; in.read(reinterpret_cast<char*>(&v), 1); return v < 0 ? 0 : static_cast<uint64_t>(v); }
    case 3: { int16_t v = 0; in.read(reinterpret_cast<char*>(&v), 2); return v < 0 ? 0 : static_cast<uint64_t>(v); }
    case 5: { int32_t v = 0; in.read(reinterpret_cast<char*>(&v), 4); return v < 0 ? 0 : static_cast<uint64_t>(v); }
    case 11: { int64_t v = 0; in.read(reinterpret_cast<char*>(&v), 8); return v < 0 ? 0 : static_cast<uint64_t>(v); }
    default:
        ok = false;
        skip_gguf_value(in, type);
        return 0;
    }
}

void skip_gguf_value(std::ifstream& in, uint32_t type) {
    switch (type) {
    case 0: case 1: case 7: skip_bytes(in, 1); break;
    case 2: case 3: skip_bytes(in, 2); break;
    case 4: case 5: case 6: skip_bytes(in, 4); break;
    case 10: case 11: case 12: skip_bytes(in, 8); break;
    case 8: {
        (void)read_gguf_string(in);
        break;
    }
    case 9: {
        uint32_t elem_type = read_u32(in);
        uint64_t n = read_u64(in);
        for (uint64_t i = 0; i < n; ++i) {
            skip_gguf_value(in, elem_type);
        }
        break;
    }
    default:
        throw std::runtime_error("Unsupported GGUF metadata value type");
    }
}

struct GgufMetadata {
    std::string architecture;
    std::map<std::string, uint64_t> u64;
    std::map<std::string, std::vector<uint64_t>> u64_arrays;
    std::map<std::string, std::vector<bool>> bool_arrays;
};

GgufMetadata parse_gguf_metadata(const std::string& gguf_path) {
    GgufMetadata meta;
    std::ifstream in(utils::path_from_utf8(gguf_path), std::ios::binary);
    if (!in.is_open()) {
        return meta;
    }

    char magic[4] = {};
    in.read(magic, 4);
    if (!in || std::memcmp(magic, "GGUF", 4) != 0) {
        return meta;
    }

    (void)read_u32(in); // version
    (void)read_u64(in); // tensor count
    uint64_t kv_count = read_u64(in);

    for (uint64_t i = 0; i < kv_count; ++i) {
        std::string key = read_gguf_string(in);
        uint32_t type = read_u32(in);

        if (key == "general.architecture" && type == 8) {
            meta.architecture = read_gguf_string(in);
            continue;
        }

        if (type == 9) {
            uint32_t elem_type = read_u32(in);
            uint64_t n = read_u64(in);
            if (n > (1ULL << 24)) {
                throw std::runtime_error("Invalid GGUF array length");
            }
            if (elem_type == 7) { // bool
                std::vector<bool> values;
                values.reserve(static_cast<size_t>(n));
                for (uint64_t j = 0; j < n; ++j) {
                    uint8_t v = 0;
                    in.read(reinterpret_cast<char*>(&v), 1);
                    if (!in) throw std::runtime_error("Unexpected end of GGUF metadata");
                    values.push_back(v != 0);
                }
                meta.bool_arrays[key] = std::move(values);
            } else if (elem_type == 0 || elem_type == 1 || elem_type == 2 || elem_type == 3 ||
                       elem_type == 4 || elem_type == 5 || elem_type == 10 || elem_type == 11) {
                std::vector<uint64_t> values;
                values.reserve(static_cast<size_t>(n));
                for (uint64_t j = 0; j < n; ++j) {
                    bool ok = false;
                    uint64_t value = read_scalar_or_skip(in, elem_type, ok);
                    if (!ok) throw std::runtime_error("Unexpected non-scalar GGUF array element");
                    values.push_back(value);
                }
                meta.u64_arrays[key] = std::move(values);
            } else {
                for (uint64_t j = 0; j < n; ++j) {
                    skip_gguf_value(in, elem_type);
                }
            }
            continue;
        }

        bool ok = false;
        uint64_t value = read_scalar_or_skip(in, type, ok);
        if (ok) {
            meta.u64[key] = value;
        }
    }

    return meta;
}

uint64_t get_u64(const GgufMetadata& meta, const std::string& suffix, uint64_t fallback = 0) {
    if (!meta.architecture.empty()) {
        auto it = meta.u64.find(meta.architecture + "." + suffix);
        if (it != meta.u64.end()) return it->second;
    }
    auto it = meta.u64.find(suffix);
    if (it != meta.u64.end()) return it->second;
    return fallback;
}

std::vector<bool> get_bool_array(const GgufMetadata& meta, const std::string& suffix) {
    if (!meta.architecture.empty()) {
        auto it = meta.bool_arrays.find(meta.architecture + "." + suffix);
        if (it != meta.bool_arrays.end()) return it->second;
    }
    auto it = meta.bool_arrays.find(suffix);
    if (it != meta.bool_arrays.end()) return it->second;
    return {};
}

uint64_t estimate_kv_cache_bytes_per_token(const GgufMetadata& meta) {
    uint64_t n_layer = get_u64(meta, "block_count");
    uint64_t n_embd = get_u64(meta, "embedding_length");
    uint64_t n_head = get_u64(meta, "attention.head_count", 1);
    uint64_t n_head_kv = get_u64(meta, "attention.head_count_kv", n_head);
    uint64_t key_length = get_u64(meta, "attention.key_length", 0);
    uint64_t value_length = get_u64(meta, "attention.value_length", 0);
    uint64_t full_attention_interval = get_u64(meta, "full_attention_interval", 0);
    uint64_t sliding_window = get_u64(meta, "attention.sliding_window", 0);
    uint64_t shared_kv_layers = get_u64(meta, "attention.shared_kv_layers", 0);

    if (n_layer == 0 || n_embd == 0) {
        return 64ULL * KiB;
    }

    if (n_head == 0) n_head = 1;
    if (n_head_kv == 0) n_head_kv = n_head;
    uint64_t default_head_dim = std::max<uint64_t>(1, n_embd / n_head);
    if (key_length == 0) key_length = default_head_dim;
    if (value_length == 0) value_length = default_head_dim;

    uint64_t kv_layer_count = n_layer;
    if (full_attention_interval > 1) {
        kv_layer_count = (n_layer + full_attention_interval - 1) / full_attention_interval;
    } else if (sliding_window > 0 && shared_kv_layers > 0 && shared_kv_layers < n_layer) {
        const uint64_t physical_kv_layers = n_layer - shared_kv_layers;
        std::vector<bool> pattern = get_bool_array(meta, "attention.sliding_window_pattern");
        if (!pattern.empty()) {
            uint64_t non_swa_pattern_entries = 0;
            for (bool is_swa : pattern) {
                if (!is_swa) ++non_swa_pattern_entries;
            }
            if (non_swa_pattern_entries > 0) {
                kv_layer_count = std::max<uint64_t>(1,
                    (physical_kv_layers * non_swa_pattern_entries + pattern.size() - 1) /
                    static_cast<uint64_t>(pattern.size()));
            } else {
                kv_layer_count = 1;
            }
        } else {
            kv_layer_count = std::max<uint64_t>(1, physical_kv_layers / 5ULL);
        }
    }

    constexpr uint64_t bytes_per_f16 = 2;
    uint64_t per_layer = saturating_mul(n_head_kv,
        saturating_add(saturating_mul(key_length, bytes_per_f16),
                       saturating_mul(value_length, bytes_per_f16)));
    uint64_t per_token = saturating_mul(kv_layer_count, per_layer);

    return saturating_add(per_token, per_token / 10);
}

uint64_t parse_meminfo_kb(const std::string& key) {
#ifndef _WIN32
#ifndef __APPLE__
    std::ifstream meminfo("/proc/meminfo");
    std::string name;
    uint64_t value = 0;
    std::string unit;
    while (meminfo >> name >> value >> unit) {
        if (name == key + ":") {
            return value;
        }
    }
#endif
#endif
    return 0;
}

uint64_t gib_to_bytes(double gib) {
    if (gib <= 0.0) {
        return 0;
    }
    long double bytes = static_cast<long double>(gib) * static_cast<long double>(GiB);
    if (bytes >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(bytes);
}

uint64_t detected_discrete_gpu_memory_bytes() {
#ifdef __APPLE__
    // Apple Metal devices normally use unified memory from the system pool.
    return 0;
#else
    try {
        auto system_info = create_system_info();
        uint64_t max_vram = 0;
        auto consider = [&max_vram](const GPUInfo& gpu) {
            if (!gpu.available || gpu.vram_gb < 1.0) {
                return;
            }
            max_vram = std::max(max_vram, gib_to_bytes(gpu.vram_gb));
        };

        for (const auto& gpu : system_info->get_amd_dgpu_devices()) {
            consider(gpu);
        }
        for (const auto& gpu : system_info->get_nvidia_gpu_devices()) {
            consider(gpu);
        }
        return max_vram;
    } catch (const std::exception&) {
        // If detection fails, prefer the safer unified-memory assumption.
    }
    return 0;
#endif
}

uint64_t host_base_bytes_for_discrete_gpu(uint64_t weight_bytes, uint64_t base_required_bytes) {
    // On dGPU systems the weights primarily live in VRAM. Host RAM still needs
    // room for process/runtime structures and mmap/page-table pressure, but it
    // should not be charged as if all model weights were resident in system RAM.
    uint64_t host_floor = saturating_add(512ULL * MiB, weight_bytes / 32ULL);
    if (base_required_bytes == 0) return host_floor;
    return std::min(base_required_bytes, host_floor);
}

void apply_memory_domain_budget(ModelMemoryEstimate& estimate, MemoryBackendClass backend) {
    estimate.host_base_required_bytes = estimate.base_required_bytes;
    estimate.device_base_required_bytes = 0;
    estimate.device_total_bytes = 0;
    estimate.host_kv_cache_bytes_per_token = estimate.kv_cache_bytes_per_token;
    estimate.separate_device_memory = false;

    switch (backend) {
    case MemoryBackendClass::CPU:
        estimate.memory_domain = "system_ram";
        break;
    case MemoryBackendClass::NPU:
        // Current AMD NPU paths use host/unified memory rather than a separate
        // large VRAM pool, so ram_limit and hard preflight must apply here.
        estimate.memory_domain = "system_ram_npu";
        break;
    case MemoryBackendClass::GPU: {
        const uint64_t device_total = detected_discrete_gpu_memory_bytes();
        if (device_total > 0) {
            estimate.separate_device_memory = true;
            estimate.memory_domain = "discrete_gpu_device_memory";
            estimate.device_base_required_bytes = estimate.base_required_bytes;
            estimate.device_total_bytes = device_total;
            estimate.host_base_required_bytes = host_base_bytes_for_discrete_gpu(
                estimate.weight_bytes, estimate.base_required_bytes);
            // The GPU backend asks llama.cpp to fit context/KV against device
            // memory. Do not reduce host RAM limits as if KV were in system RAM.
            estimate.host_kv_cache_bytes_per_token = 0;
        } else {
            estimate.memory_domain = "unified_gpu_memory";
        }
        break;
    }
    }
}

uint64_t host_base_required_or_fallback(const ModelMemoryEstimate& estimate) {
    return estimate.host_base_required_bytes > 0
        ? estimate.host_base_required_bytes
        : estimate.base_required_bytes;
}

uint64_t host_kv_bytes_per_token_or_fallback(const ModelMemoryEstimate& estimate) {
    if (estimate.separate_device_memory) {
        return estimate.host_kv_cache_bytes_per_token;
    }
    return estimate.host_kv_cache_bytes_per_token > 0
        ? estimate.host_kv_cache_bytes_per_token
        : estimate.kv_cache_bytes_per_token;
}

void apply_base_preflight_check(ModelMemoryEstimate& estimate, int64_t ram_limit_mib) {
    SystemMemoryProbe probe = MemoryManager::probe_system_memory(ram_limit_mib);
    std::vector<std::string> errors;

    const uint64_t host_required = host_base_required_or_fallback(estimate);
    if (probe.effective_available_bytes > 0 &&
        host_required > probe.effective_available_bytes) {
        std::ostringstream oss;
        oss << "Insufficient system RAM to load base model footprint. Required "
            << MemoryManager::format_bytes(host_required)
            << ", available/allowed "
            << MemoryManager::format_bytes(probe.effective_available_bytes) << ".";
        errors.push_back(oss.str());
    }

    if (estimate.separate_device_memory &&
        estimate.device_total_bytes > 0 &&
        estimate.device_base_required_bytes > estimate.device_total_bytes) {
        std::ostringstream oss;
        oss << "Insufficient discrete GPU memory to load base model footprint. Required "
            << MemoryManager::format_bytes(estimate.device_base_required_bytes)
            << ", detected total GPU memory "
            << MemoryManager::format_bytes(estimate.device_total_bytes) << ".";
        errors.push_back(oss.str());
    }

    if (!errors.empty()) {
        estimate.hard_error = true;
        std::ostringstream oss;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i > 0) oss << " ";
            oss << errors[i];
        }
        if (estimate.separate_device_memory) {
            oss << " Host estimate "
                << MemoryManager::format_bytes(host_required)
                << ", device estimate "
                << MemoryManager::format_bytes(estimate.device_base_required_bytes)
                << ".";
        }
        estimate.warning = oss.str();
    }
}

} // namespace

uint64_t ModelMemoryEstimate::total_required_bytes() const {
    uint64_t host_base = host_base_required_bytes > 0
        ? host_base_required_bytes
        : base_required_bytes;
    uint64_t host_kv = separate_device_memory
        ? host_kv_cache_bytes_per_token
        : (host_kv_cache_bytes_per_token > 0
            ? host_kv_cache_bytes_per_token
            : kv_cache_bytes_per_token);
    return saturating_add(host_base,
                          saturating_mul(host_kv,
                                         static_cast<uint64_t>(std::max(0, final_context))));
}

json ModelMemoryEstimate::to_json() const {
    json result = {
        {"weight_bytes", weight_bytes},
        {"overhead_bytes", overhead_bytes},
        {"base_required_bytes", base_required_bytes},
        {"host_base_required_bytes", host_base_required_bytes},
        {"device_base_required_bytes", device_base_required_bytes},
        {"device_total_bytes", device_total_bytes},
        {"kv_cache_bytes_per_token", kv_cache_bytes_per_token},
        {"host_kv_cache_bytes_per_token", host_kv_cache_bytes_per_token},
        {"separate_device_memory", separate_device_memory},
        {"memory_domain", memory_domain},
        {"model_max_context", model_max_context},
        {"context_target", target_context},
        {"probe_context", probe_context},
        {"final_context", final_context},
        {"dynamic_context", dynamic_context},
        {"hard_error", hard_error},
        {"restricted_context_warning", restricted_context_warning},
        {"user_context_override", user_context_override},
        {"model_context_limit_is_trustworthy", model_context_limit_is_trustworthy}
    };
    if (!warning.empty()) result["warning"] = warning;
    if (!detail.empty()) result["detail"] = detail;
    return result;
}

uint64_t MemoryManager::ram_limit_mib_to_bytes(int64_t ram_limit_mib) {
    if (ram_limit_mib < 0) return 0;
    return saturating_mul(static_cast<uint64_t>(ram_limit_mib), MiB);
}

SystemMemoryProbe MemoryManager::probe_system_memory(int64_t ram_limit_mib) {
    SystemMemoryProbe probe;
    probe.ram_limit_bytes = ram_limit_mib_to_bytes(ram_limit_mib);

#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        probe.total_bytes = static_cast<uint64_t>(status.ullTotalPhys);
        probe.available_bytes = static_cast<uint64_t>(status.ullAvailPhys);
        probe.source = "GlobalMemoryStatusEx";
    }
#elif defined(__APPLE__)
    uint64_t memsize = 0;
    size_t memsize_len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &memsize_len, nullptr, 0) == 0) {
        probe.total_bytes = memsize;
    }

    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vmstat;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmstat), &count) == KERN_SUCCESS) {
        uint64_t page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        uint64_t pages = static_cast<uint64_t>(vmstat.free_count) +
                         static_cast<uint64_t>(vmstat.inactive_count) +
                         static_cast<uint64_t>(vmstat.speculative_count);
        probe.available_bytes = saturating_mul(pages, page_size);
        probe.source = "host_statistics64";
    }
#else
    uint64_t mem_available_kb = parse_meminfo_kb("MemAvailable");
    uint64_t mem_total_kb = parse_meminfo_kb("MemTotal");
    if (mem_available_kb > 0) {
        probe.available_bytes = saturating_mul(mem_available_kb, KiB);
        probe.total_bytes = saturating_mul(mem_total_kb, KiB);
        probe.source = "/proc/meminfo:MemAvailable";
    } else {
        long pages = sysconf(_SC_AVPHYS_PAGES);
        long total_pages = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0) {
            probe.available_bytes = saturating_mul(static_cast<uint64_t>(pages),
                                                   static_cast<uint64_t>(page_size));
        }
        if (total_pages > 0 && page_size > 0) {
            probe.total_bytes = saturating_mul(static_cast<uint64_t>(total_pages),
                                               static_cast<uint64_t>(page_size));
        }
        probe.source = "sysconf";
    }
#endif

    probe.effective_available_bytes = probe.available_bytes;
    if (probe.available_bytes == 0 && probe.ram_limit_bytes > 0) {
        // If the OS probe failed but the user provided a deterministic runtime
        // limit, use that limit rather than turning the preflight into an
        // unconditional false-positive failure.
        probe.effective_available_bytes = probe.ram_limit_bytes;
    } else if (probe.ram_limit_bytes > 0 && probe.ram_limit_bytes < probe.effective_available_bytes) {
        probe.effective_available_bytes = probe.ram_limit_bytes;
    }
    return probe;
}

uint64_t MemoryManager::get_weight_size_bytes(const ModelInfo& model_info,
                                              const std::string& gguf_path) {
    uint64_t total = 0;

    total = file_size_bytes(gguf_path);
    if (total == 0 && model_info.size > 0.0) {
        total = static_cast<uint64_t>(std::ceil(model_info.size * static_cast<double>(GiB)));
    }

    // Multimodal llama.cpp models load an additional mmproj GGUF next to the
    // main model. Count it as part of the base footprint so ram_limit preflight
    // and strict context capping are not overly optimistic for vision/audio
    // models. The main/auxiliary paths may be absent for text-only models.
    const std::string mmproj_path = model_info.resolved_path("mmproj");
    if (!mmproj_path.empty() && mmproj_path != gguf_path) {
        total = saturating_add(total, file_size_bytes(mmproj_path));
    }

    return total;
}

uint64_t MemoryManager::backend_overhead_bytes(uint64_t weight_bytes,
                                               MemoryBackendClass backend) {
    switch (backend) {
    case MemoryBackendClass::CPU:
        return saturating_add(256ULL * MiB, weight_bytes / 16);  // ~6.25%
    case MemoryBackendClass::GPU:
        return saturating_add(768ULL * MiB, weight_bytes / 8);   // ~12.5%
    case MemoryBackendClass::NPU:
        return saturating_add(1024ULL * MiB, weight_bytes / 7);  // ~14.3%
    }
    return 512ULL * MiB;
}

ModelMemoryEstimate MemoryManager::estimate_llamacpp_memory(
    const ModelInfo& model_info,
    const std::string& gguf_path,
    MemoryBackendClass backend,
    int context_target,
    int64_t ram_limit_mib) {

    ModelMemoryEstimate estimate;
    estimate.dynamic_context = context_target > 0;
    estimate.target_context = context_target > 0 ? context_target : 0;
    estimate.probe_context = kProbeContext;
    estimate.weight_bytes = get_weight_size_bytes(model_info, gguf_path);
    estimate.overhead_bytes = backend_overhead_bytes(estimate.weight_bytes, backend);
    estimate.base_required_bytes = saturating_add(estimate.weight_bytes, estimate.overhead_bytes);

    GgufMetadata meta;
    try {
        meta = parse_gguf_metadata(gguf_path);
    } catch (const std::exception& e) {
        estimate.detail = std::string("GGUF metadata parse failed; using conservative KV estimate: ") + e.what();
    }

    estimate.kv_cache_bytes_per_token = estimate_kv_cache_bytes_per_token(meta);
    uint64_t ctx = get_u64(meta, "context_length", 0);
    if (ctx > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        ctx = static_cast<uint64_t>(std::numeric_limits<int>::max());
    }
    estimate.model_max_context = static_cast<int>(ctx);
    estimate.model_context_limit_is_trustworthy = ctx > 0;

    apply_memory_domain_budget(estimate, backend);

    apply_base_preflight_check(estimate, ram_limit_mib);

    return estimate;
}

uint64_t MemoryManager::estimate_loaded_model_bytes(const ModelMemoryEstimate& estimate) {
    return estimate.total_required_bytes();
}

ModelMemoryEstimate MemoryManager::estimate_non_llamacpp_memory(
    const ModelInfo& model_info,
    MemoryBackendClass backend,
    int context_target,
    int64_t ram_limit_mib) {

    ModelMemoryEstimate estimate;
    estimate.dynamic_context = false;
    estimate.target_context = context_target > 0 ? context_target : 0;
    estimate.final_context = context_target > 0 ? context_target : 0;
    estimate.weight_bytes = get_weight_size_bytes(model_info, model_info.resolved_path());
    estimate.overhead_bytes = backend_overhead_bytes(estimate.weight_bytes, backend);
    estimate.base_required_bytes = saturating_add(estimate.weight_bytes, estimate.overhead_bytes);
    estimate.kv_cache_bytes_per_token = 0;
    apply_memory_domain_budget(estimate, backend);

    apply_base_preflight_check(estimate, ram_limit_mib);

    return estimate;
}

uint64_t MemoryManager::estimate_non_llamacpp_loaded_bytes(const ModelInfo& model_info,
                                                           MemoryBackendClass backend) {
    return estimate_non_llamacpp_memory(model_info, backend, 0, -1).total_required_bytes();
}

int MemoryManager::get_llamacpp_model_max_context(const std::string& gguf_path) {
    if (gguf_path.empty()) {
        return 0;
    }

    try {
        GgufMetadata meta = parse_gguf_metadata(gguf_path);
        uint64_t ctx = get_u64(meta, "context_length", 0);
        if (ctx > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            ctx = static_cast<uint64_t>(std::numeric_limits<int>::max());
        }
        return static_cast<int>(ctx);
    } catch (...) {
        return 0;
    }
}

std::string MemoryManager::get_llamacpp_architecture(const std::string& gguf_path) {
    if (gguf_path.empty()) {
        return {};
    }

    try {
        GgufMetadata meta = parse_gguf_metadata(gguf_path);
        return meta.architecture;
    } catch (...) {
        return {};
    }
}


std::string MemoryManager::backend_to_string(MemoryBackendClass backend) {
    switch (backend) {
    case MemoryBackendClass::CPU: return "cpu";
    case MemoryBackendClass::GPU: return "gpu";
    case MemoryBackendClass::NPU: return "npu";
    }
    return "unknown";
}

std::string MemoryManager::format_bytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (bytes >= GiB) {
        oss << (static_cast<double>(bytes) / static_cast<double>(GiB)) << " GiB";
    } else if (bytes >= MiB) {
        oss << (static_cast<double>(bytes) / static_cast<double>(MiB)) << " MiB";
    } else if (bytes >= KiB) {
        oss << (static_cast<double>(bytes) / static_cast<double>(KiB)) << " KiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

} // namespace lemon
