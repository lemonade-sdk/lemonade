// AutoOpt wizard data model: wizard answers, probe measurements, and the
// synthesized presets. Shared by the pure decision engine (auto_opt_engine.h),
// the probe runners, the run manager, and the HTTP handlers.
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace autoopt {

using json = nlohmann::ordered_json;

enum class BudgetTier { Quick, Standard, Thorough };

inline std::string to_string(BudgetTier t) {
    switch (t) {
        case BudgetTier::Quick: return "quick";
        case BudgetTier::Standard: return "standard";
        default: return "thorough";
    }
}

inline std::optional<BudgetTier> budget_from_string(const std::string& s) {
    if (s == "quick") return BudgetTier::Quick;
    if (s == "standard") return BudgetTier::Standard;
    if (s == "thorough") return BudgetTier::Thorough;
    return std::nullopt;
}

struct WizardAnswers {
    bool parallel = false;
    int slots = 1;
    bool dedicated_slots = true;      // -no-kvu (guaranteed ctx/slot) vs -kvu shared pool
    std::string kv_cache_quant = "none";   // none | q8_0 | q5_1 | q4_0
    std::string ram_headroom = "normal";   // normal | reduced | minimal | disabled
    std::optional<bool> use_vision;
    bool allow_network = true;
    std::vector<std::string> backends_to_consider;

    static WizardAnswers from_json(const json& j) {
        WizardAnswers a;
        if (j.contains("parallel") && j["parallel"].is_object()) {
            const auto& p = j["parallel"];
            a.parallel = p.value("mode", "single") == "parallel";
            a.slots = std::max(1, p.value("slots", a.parallel ? 2 : 1));
            a.dedicated_slots = p.value("dedicated", true);
        }
        a.kv_cache_quant = j.value("kv_cache_quant", "none");
        a.ram_headroom = j.value("ram_headroom", "normal");
        if (j.contains("use_vision") && j["use_vision"].is_boolean())
            a.use_vision = j["use_vision"].get<bool>();
        a.allow_network = j.value("allow_network", true);
        if (j.contains("backends_to_consider") && j["backends_to_consider"].is_array())
            for (const auto& b : j["backends_to_consider"])
                if (b.is_string()) a.backends_to_consider.push_back(b.get<std::string>());
        return a;
    }

    json to_json() const {
        json p = {{"mode", parallel ? "parallel" : "single"},
                  {"slots", slots},
                  {"dedicated", dedicated_slots}};
        json j = {{"parallel", p},
                  {"kv_cache_quant", kv_cache_quant},
                  {"ram_headroom", ram_headroom},
                  {"allow_network", allow_network}};
        if (use_vision) j["use_vision"] = *use_vision;
        if (!backends_to_consider.empty()) j["backends_to_consider"] = backends_to_consider;
        return j;
    }
};

struct HardwareSnapshot {
    struct Gpu {
        std::string vendor;           // "amd" | "nvidia"
        std::string name;
        std::string family;           // sm_XX / gfxXXXX
        double vram_gb = 0;
    };
    std::vector<Gpu> gpus;
    bool has_igpu = false;
    bool ram_is_vram = false;         // unified memory (iGPU/APU): VRAM budget == RAM
    double host_ram_gb = 0;
    double host_ram_available_gb = 0;
    double gpu_available_gb = 0;
    std::vector<std::string> installed_backends;   // llamacpp variants, e.g. {"vulkan","rocm-stable"}
    std::string os;
};

struct ModelFacts {
    std::string architecture;
    int64_t block_count = 0;
    int64_t expert_count = 0;
    int64_t full_attention_interval = 0;
    int64_t swa_layer_count = 0;
    int64_t n_ctx_train = 0;
    double file_size_gb = 0;
    double kv_bytes_per_token = 0;
    bool is_moe = false;
    bool is_hybrid_or_recurrent = false;
    bool has_mtp = false;
    bool has_vision = false;
    std::string gguf_path;
    std::string mmproj_path;
    std::string base_model_repo;
};

struct FitEstimate {
    std::string backend;
    int fit_target_mib = 1024;
    std::string extra_args;           // probe variant, e.g. "-ctk q8_0 -ctv q8_0" or "-ncmoe 12"
    std::string fitted_args;          // stdout line 1, e.g. "-c 32768 -ngl -1"
    int fitted_ctx = 0;
    int fitted_ngl = -1;              // -1 = full offload
    int fitted_ncmoe = 0;
    struct DeviceMem {
        std::string device;
        int model_mib = 0, ctx_mib = 0, compute_mib = 0;
    };
    std::vector<DeviceMem> devices;
    bool fits_fully = false;
    bool ok = false;
    std::string error;

    int total_mib() const {
        int t = 0;
        for (const auto& d : devices)
            if (d.device.rfind("Host", 0) != 0) t += d.model_mib + d.ctx_mib + d.compute_mib;
        return t;
    }

    json to_json() const {
        json devs = json::array();
        for (const auto& d : devices)
            devs.push_back({{"device", d.device}, {"model_mib", d.model_mib},
                            {"ctx_mib", d.ctx_mib}, {"compute_mib", d.compute_mib}});
        return {{"backend", backend}, {"fit_target_mib", fit_target_mib},
                {"extra_args", extra_args}, {"fitted_args", fitted_args},
                {"fitted_ctx", fitted_ctx}, {"fitted_ngl", fitted_ngl},
                {"fitted_ncmoe", fitted_ncmoe}, {"devices", devs},
                {"fits_fully", fits_fully}, {"ok", ok}, {"error", error}};
    }
};

struct BenchPoint {
    std::string backend;
    json params;                      // varied flags, e.g. {"d":30000,"b":2048,"ub":2048,"spec_n":3}
    double pp_avg_ts = 0;
    double tg_avg_ts = 0;
    int n_depth = 0;
    bool ok = false;
    std::string error;

    json to_json() const {
        return {{"backend", backend}, {"params", params}, {"pp_avg_ts", pp_avg_ts},
                {"tg_avg_ts", tg_avg_ts}, {"n_depth", n_depth}, {"ok", ok}, {"error", error}};
    }
};

struct SamplingDefaults {
    std::optional<double> temperature, top_p, min_p;
    std::optional<int> top_k;
    std::string source;

    json to_json() const {
        json j = {{"source", source}};
        if (temperature) j["temperature"] = *temperature;
        if (top_p) j["top_p"] = *top_p;
        if (min_p) j["min_p"] = *min_p;
        if (top_k) j["top_k"] = *top_k;
        return j;
    }
};

struct GeneratedPreset {
    std::string label;
    std::string tradeoff;
    std::string llamacpp_backend;
    int ctx_size = -1;
    bool mmproj_enabled = true;
    std::string llamacpp_args;
    std::vector<std::string> rationale;
    json expected = json::object();   // {"pp_ts":..,"tg_ts":..,"vram_mib":..} when measured

    json to_json() const {
        json j = {{"label", label},
                  {"llamacpp_backend", llamacpp_backend},
                  {"ctx_size", ctx_size},
                  {"mmproj_enabled", mmproj_enabled},
                  {"llamacpp_args", llamacpp_args},
                  {"rationale", rationale}};
        if (!tradeoff.empty()) j["tradeoff"] = tradeoff;
        if (!expected.empty()) j["expected"] = expected;
        return j;
    }
};

struct AutoOptResult {
    GeneratedPreset primary;
    std::vector<GeneratedPreset> alternatives;
    std::optional<SamplingDefaults> sampling_defaults;

    json to_json() const {
        json alts = json::array();
        for (const auto& a : alternatives) alts.push_back(a.to_json());
        json j = {{"primary", primary.to_json()}, {"alternatives", alts}};
        if (sampling_defaults) j["sampling_defaults"] = sampling_defaults->to_json();
        return j;
    }
};

}  // namespace autoopt
}  // namespace lemon
