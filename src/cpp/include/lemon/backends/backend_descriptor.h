#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "lemon/model_types.h"
#include "lemon/recipe_backend_def.h"

namespace lemon {

// A single declarative configuration knob a backend exposes. The same list
// drives config.json defaults, CLI flag registration, and load-time option
// resolution, so they can never drift apart.
struct BackendOption {
    std::string name;                 // option key, e.g. "vllm_args"
    std::string cli_flag;             // CLI flag, e.g. "--vllm-args" ("" = not a CLI flag)
    nlohmann::json default_value;     // default value when the option is unset
    std::string type_name;            // "ARGS" | "SIZE" | "BACKEND" | "BOOL"
    std::string help;                 // CLI help text
    std::string group;                // CLI help group, e.g. "General Options"
};

// How a backend shares the accelerator.
enum class SlotPolicy {
    Standard,      // counts toward the LRU slots, no device exclusivity (llamacpp, sd-cpp)
    ExclusiveNpu,  // evict ALL npu servers before loading (ryzenai-llm, whispercpp-npu)
    CoexistByType, // one per model type, evicts exclusive-npu peers (flm)
    Unmetered      // never counts toward slots, never auto-evicted (cloud)
};

// How an installed backend version is compared against the expected pin.
enum class VersionPolicy {
    Exact,    // installed must match the expected version
    AtLeast   // installed >= expected is acceptable (system-managed packages, e.g. flm)
};

inline const char* slot_policy_to_string(SlotPolicy p) {
    switch (p) {
        case SlotPolicy::Standard:      return "standard";
        case SlotPolicy::ExclusiveNpu:  return "exclusive_npu";
        case SlotPolicy::CoexistByType: return "coexist_by_type";
        case SlotPolicy::Unmetered:     return "unmetered";
    }
    return "standard";
}

// Plain data declaring *what a backend is*. This is the single object the
// registry, the CLI, /system-info, and the docs all read. Behavior lives in the
// paired WrappedServer subclass (see backend_registry.h for how they bind).
struct BackendDescriptor {
    std::string recipe;             // "vllm"
    std::string display_name;       // "vLLM ROCm (experimental)"
    std::string binary;             // subprocess to launch/install ("" = none, e.g. cloud)
    std::string config_section;     // config.json section; defaults to recipe (sd-cpp -> "sdcpp")

    DeviceType default_device = DEVICE_GPU;           // default; override effective_device() if variant-dependent
    SlotPolicy slot_policy    = SlotPolicy::Standard; // default; override effective_slot_policy() if variant-dependent
    bool selectable_backend   = false;  // auto-creates "<recipe>_backend" option + "--<recipe>" flag
    bool uses_ctx_size        = false;  // opt in to the shared ctx_size option
    bool dynamic_models       = false;  // true = ops supply models at runtime (cloud, flm), not server_models.json

    std::vector<BackendOption>    options;                       // backend-specific knobs (common ones are automatic)
    std::vector<BackendSupport>   support;                       // which OS / GPU families it runs on ({} = no local gating)
    std::vector<std::string>      default_labels;                // labels injected when a model omits them
    std::vector<std::string>      required_checkpoints{"main"};  // unconditional files; conditional ones checked in load()

    // Editorial metadata for the generated docs (README support matrix, website).
    std::string modality;           // "Text generation" | "Speech-to-text" | "Text-to-speech" | "Image generation"
    bool        experimental = false; // true renders "(experimental)" next to the recipe in generated docs
    std::string web_display_name;   // name used on the docs website ("" = fall back to display_name)

    // ROCm release channels this backend publishes (e.g. {"stable","nightly"}).
    // Empty = the backend has no ROCm channels (its "rocm" build is a single
    // artifact). Drives the rocm-stable/rocm-nightly bin-key collapse and the
    // channel clamp (a requested channel not listed here falls back to the first).
    std::vector<std::string> rocm_channels;

    // True if the backend's subprocess exposes a Prometheus /metrics endpoint
    // that lemond should scrape and re-export (llama-server does).
    bool exposes_prometheus_metrics = false;

    // True if this backend's ROCm build requires the gfx1151 (Strix Halo) kernel
    // CWSR fix. Gates the availability/remediation check for the "rocm" backend.
    bool rocm_requires_cwsr_fix = false;

    // How the installed version is compared against the expected pin. Exact by
    // default; system-managed packages (flm) accept any version >= expected.
    VersionPolicy version_policy = VersionPolicy::Exact;

    // True if the backend pulls its own models on demand (flm self-pulls via its
    // CLI) rather than being pre-downloaded from Hugging Face by the router. Such
    // backends are skipped by the load-time auto-download path.
    bool self_manages_downloads = false;

    // --- config.json per-recipe defaults schema ---
    // The backend's section of config.json is derived from these fields, so a new
    // backend's defaults live in its descriptor instead of a hand-maintained
    // defaults.json block. (selectable_backend additionally emits `backend: "auto"`.)
    bool takes_args = false;                       // emits `args: ""`
    std::vector<std::string> arg_variants;         // each emits `<variant>_args: ""`
    std::vector<std::string> bin_variants;         // each emits `<variant>_bin: "builtin"`
    nlohmann::json config_extra = nlohmann::json::object();  // fixed extras (e.g. prefer_system, image defaults)

    // The config.json section name for this backend, falling back to the recipe.
    std::string effective_config_section() const {
        return config_section.empty() ? recipe : config_section;
    }

    // Build this backend's config.json default section from the schema above.
    // Returns an empty object when the backend has no configurable section.
    nlohmann::json config_defaults() const {
        nlohmann::json block = nlohmann::json::object();
        if (selectable_backend) block["backend"] = "auto";
        if (takes_args) block["args"] = "";
        for (const auto& v : arg_variants) block[v + "_args"] = "";
        for (const auto& v : bin_variants) block[v + "_bin"] = "builtin";
        if (config_extra.is_object()) {
            for (auto it = config_extra.begin(); it != config_extra.end(); ++it) {
                block[it.key()] = it.value();
            }
        }
        return block;
    }
};

} // namespace lemon
