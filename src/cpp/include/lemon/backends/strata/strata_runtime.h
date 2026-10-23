#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lemon::backends::strata {

std::set<std::string> engine_data_files();
bool engine_data_present(const std::filesystem::path& directory, const std::string& backend);
void prepare_engine_data(const std::filesystem::path& directory, const std::string& backend,
    const std::function<void(const std::string&, const std::filesystem::path&)>& download);

struct EngineRelease {
    std::string repository;
    std::string asset_pattern;
    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> targets;
    std::vector<std::string> arguments;
    int default_context;
    int kv_short_context;
    std::string kv_short_format;
    std::string kv_long_format;
    std::map<std::string, std::set<std::string>> kv_formats;
    int kv_layers;
    std::map<std::string, int> kv_cell_bytes;
    int kv_streaming_context;
    int kv_resident_context;
    double kv_streaming_headroom_gb;
    double vram_small_gpu_gib;
    int vram_small_reserve_mib;
    int vram_large_reserve_mib;
    double vram_reserve_fraction;
    int vision_cpu_max_tokens;
    int vision_gpu_max_tokens;
    int vision_gpu_reserve_mib;
    int expert_ram_headroom_gib;
    int expert_resident_headroom_gib;
    int expert_min_budget_gib;
    int pool_workers_cap;

    std::string asset(const std::string& os, const std::string& arch,
                      const std::string& backend, const std::string& gpu_arch = "") const;
    std::string data_url(const std::string& tag, const std::string& file) const;
};

struct DraftFile {
    std::string source;
};

struct DraftPack {
    std::string repository;
    std::string revision;
    std::set<std::string> backends;
    std::map<std::string, DraftFile> files;
    std::vector<std::string> arguments;

    std::filesystem::path directory() const;
    nlohmann::json identity() const;
    bool file_present(const std::string& name) const;
    bool present() const;
};

DraftPack parse_draft_pack(const nlohmann::json& metadata);

struct VisionPack {
    std::string repository;
    std::string revision;
    std::string file;

    std::filesystem::path directory() const;
    std::filesystem::path path() const;
    nlohmann::json identity() const;
    bool present() const;
};

VisionPack parse_vision_pack(const nlohmann::json& metadata);

struct MemoryCapacity {
    double total_ram_gib = -1.0;
    double total_vram_gib = -1.0;
    bool unified_gpu = false;
    bool wsl = false;
    double expert_size_gib = 0;
    int performance_cores = 0;
    int efficiency_cores = 0;
};

struct MemoryPlan {
    std::string experts;
    bool ple_direct = false;
    int kv_resident = 0;
    int vram_reserve_mib = -1;
    int resident_budget_gib = 0;
    int pool_workers = 0;
    void apply(std::vector<std::string>& arguments) const;
};

struct ModelCheckpoint {
    std::string repository;
    std::string revision;
    std::string quant;
    std::vector<std::string> files;
    std::string profile_file;
    std::string preparation = "gguf-pack";
    bool compat_bf16 = false;
    std::set<std::string> backends;
    std::vector<std::string> arguments;
    bool replace_arguments = false;
    std::string native_file;
    std::string ple_file;
    std::string embedding_file;
    std::optional<DraftPack> mtp;
    int embedded_mtp_drafts = 0;
    std::optional<VisionPack> vision;
    bool full_expert_arena = false;
    bool budget_experts = false;
    double recommended_ram_gb = 0.0;

    std::filesystem::path directory() const;
    std::filesystem::path local_file(const std::filesystem::path& folder, const std::string& file) const;
    void validate_backend(const std::string& backend) const;
    void validate_custom_arguments(const std::string& arguments) const;
    std::vector<std::string> launch_arguments(const EngineRelease& release, const std::string& backend,
                                            const std::filesystem::path& folder,
                                            const std::filesystem::path& engine_data, int context,
                                            bool enable_mtp = true, const std::string& kv_cache = "auto") const;
    MemoryPlan memory_plan(const EngineRelease& release, const std::string& backend, int context,
                           const std::string& kv_cache, const std::string& experts,
                           const std::string& kv_streaming, const MemoryCapacity& capacity,
                           bool custom_arguments) const;
    nlohmann::json identity() const;
    bool downloads_present(const std::filesystem::path& folder) const;
};

EngineRelease parse_engine_release(const nlohmann::json& metadata);
EngineRelease engine_release();
const std::set<std::string>& managed_arguments();
ModelCheckpoint parse_model_checkpoint(const std::string& checkpoint, const nlohmann::json& models);
ModelCheckpoint model_checkpoint(const std::string& checkpoint, const std::string& backend = "");
void record_downloads(const std::filesystem::path& directory, const std::string& receipt,
                      const nlohmann::json& identity, const std::vector<std::filesystem::path>& files);

}
