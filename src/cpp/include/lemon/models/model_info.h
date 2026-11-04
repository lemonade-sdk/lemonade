#pragma once

#include <lemon/gguf_capabilities.h>
#include <lemon/model_types.h>
#include <lemon/recipe_options.h>

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lemon {

// Parsed collection.router routing policy (defined in routing_policy.h). Only
// forward-declared here so this widely-included header stays light; ModelInfo
// holds it behind a shared_ptr, which supports incomplete types.
struct RoutePolicy;

// Image generation defaults for SD models
struct ImageDefaults {
    int steps = 20;
    float cfg_scale = 7.0f;
    int width = 512;
    int height = 512;
    std::string sampling_method;
    float flow_shift = 0.0f;
    std::uint64_t soft_pixel_cap = 0;  // 0 uses the server's inferred edit-size cap

    bool has_defaults = false;  // True if explicit defaults were provided in JSON
};

/// Metadata extracted from a GGUF file's KV header.
///
/// Stored inside ModelInfo for llama.cpp models so that auto-tune and other
/// subsystems can estimate KV-cache pressure and clamp context size.
///
/// Arrays that vary per-block (head_count_kv_per_layer, sliding_window_pattern)
/// are stored as-is so that downstream code can perform precise per-layer
/// computations (e.g. distinguishing SWA-layer head counts from full-layer
/// head counts).  Scalar convenience fields (head_count_kv, swa_layer_count)
/// are derived from the raw arrays after the header pass completes.
struct GgufMetadata {
    std::string architecture;
    int64_t context_length = 0;
    int64_t block_count = 0;
    int64_t embedding_length = 0;
    int64_t head_count_kv = 0;       // total across all blocks (derived)
    int64_t key_length = 0;
    int64_t key_length_swa = 0;      // SWA-reduced key length (Gemma4, etc.)
    int64_t swa_layer_count = 0;     // layers with sliding-window attention (derived)
    int64_t full_attention_interval = 0; // every Nth layer does full attention (Qwen SSM)
    GgufCapabilities caps;

    // ── Raw per-layer arrays (stored as read from GGUF) ───────────────
    // Populated when the GGUF field is an array.  Empty when the field is
    // a scalar (in which case the scalar is repeated for every block).
    std::vector<int64_t> head_count_kv_per_layer;
    std::vector<bool> sliding_window_pattern;

    // Transient scalar held when head_count_kv GGUF field is a scalar.
    // Converted to head_count_kv_per_layer in post-loop validation.
    int64_t head_count_kv_scalar = 0;
};

struct ModelInfo {
    std::string model_name;
    std::map<std::string, std::string> checkpoints;
    std::map<std::string, std::string> resolved_paths; // Absolute path to model file/directory on disk
    std::string recipe;
    std::vector<std::string> labels;
    std::vector<std::string> components;
    std::vector<std::string> input_aliases;  // Names accepted in requests but hidden from /models
    bool suggested = false;
    std::string source;  // Local origin: local_upload/local_path/extra_models_dir
    std::string registry_source;  // Remote registry: huggingface/modelscope; empty when unpinned
    bool downloaded = false;     // Whether model is downloaded and available
    bool update_available = false; // Whether a newer remote-registry version exists
    std::optional<bool> auto_update = std::nullopt; // Optional per-model auto-update override
    double size = 0.0;   // Model size in GB
    // Resident working set for a streaming backend; 0 = filter on full `size`.
    // See streaming_working_set_gb() / filter_for_host.
    double min_resident_gb = 0.0;
    int64_t max_context_window = 0;  // Static model-supported text context, when known
    int64_t max_output_tokens = 0;    // Static model-supported completion/output token limit, when known

    // GGUF architecture metadata (populated for llamacpp models, used for auto ctx_size)
    GgufMetadata gguf;
    RecipeOptions recipe_options;

    // Multi-model support fields
    ModelType type = ModelType::LLM;      // Model type for LRU cache management
    DeviceType device = DEVICE_NONE;      // Target device(s) for this model

    // Image generation defaults (for sd-cpp models)
    ImageDefaults image_defaults;

    // Per-collection system prompt template (collection.omni models only).
    // When non-empty, overrides the global default in toolDefinitions.json.
    // Stays a template — {tool_list} / {tool_guidance} are substituted at runtime.
    std::string system_prompt;

    // Cloud offload (for "cloud" recipe). Names the provider to dispatch to
    // (e.g., "fireworks"). Empty for non-cloud recipes.
    std::string cloud_provider;
    // Per-token price in USD per 1,000,000 tokens, when the provider reports it
    // (OpenRouter, Together). <0 means unknown (e.g. Fireworks doesn't publish
    // pricing in /v1/models). Surfaced on /v1/models for display, and (when a
    // collection.router policy is evaluated) attached to the decision as
    // illustrative outputs.estimated_cost — not a billing figure.
    double cost_input_per_million = -1.0;
    double cost_output_per_million = -1.0;

    // Generic per-model fields a backend declares for itself. Any server_models.json
    // key not consumed by a typed field above lands here, so a new backend can read
    // custom per-model config in load() without editing this shared struct.
    std::map<std::string, json> extras;

    // Parsed routing policy for collection.router models. Populated once when the
    // models cache is built (from recipe + components + the "routing" block in
    // extras) so request-time dispatch reads it directly instead of re-parsing.
    // Null for every other recipe. shared_ptr keeps ModelInfo copies cheap.
    std::shared_ptr<const RoutePolicy> route_policy;

    // Look up an extra field, returning a default when absent.
    template <typename T>
    T extra(const std::string& key, const T& fallback) const {
        auto it = extras.find(key);
        if (it == extras.end() || it->second.is_null()) return fallback;
        try { return it->second.get<T>(); } catch (...) { return fallback; }
    }

    // Utility
    std::string checkpoint(const std::string& type = "main") const { return checkpoints.count(type) ? checkpoints.at(type) : ""; }
    std::string resolved_path(const std::string& type = "main") const { return resolved_paths.count(type) ? resolved_paths.at(type) : ""; }

    std::string mmproj() const { return checkpoint("mmproj"); }
};

struct ModelFileInfo {
    std::string name;
    std::string path;
    std::string role;
    std::uint64_t size_bytes = 0;
    bool exists = false;
};

}  // namespace lemon
