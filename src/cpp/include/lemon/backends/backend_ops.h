#pragma once

#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;
class ModelManager;

namespace backends {

// Context handed to BackendOps methods — the bits of server state model
// management needs without a running subprocess. Grows as migrations require.
struct BackendOpsContext {
    ModelManager* model_manager = nullptr;
};

// Inputs for resolving a checkpoint's on-disk path. The model manager computes
// the HF-cache locations generically; each backend's ops decide how to find its
// artifact within (a .gguf file, a genai_config.json directory, a .bin, …).
struct CheckpointResolveContext {
    std::string hf_cache;          // HF cache root dir
    std::string model_cache_path;  // hf_cache/<checkpoint repo cache dir>
    std::string repo_id;           // checkpoint's repo id
    std::string main_repo_id;      // the model's "main" checkpoint repo id (fallback)
    std::string variant;           // checkpoint variant after ':' ("" if none)
    std::string type;              // checkpoint type ("main", "mmproj", "npu_cache", …)
    std::string checkpoint;        // the raw checkpoint string
};

// Stateless per-backend behavior for model management that happens WITHOUT a
// running subprocess: checkpoint-path resolution, download, dynamic discovery,
// per-model metadata, version detection, availability. One singleton per
// backend, exposed via lemon::backends::<stem>::ops() and bound in the registry
// (see BackendRegistration::ops).
//
// The base class is the shared default behavior (the common HF-backed case);
// each backend folder overrides ONLY the policy points it needs, so shared
// logic is inherited rather than copied. Methods are added here incrementally as
// switchboards in model_manager / system_info are migrated; every method has a
// default so adding one never forces edits to backends that don't override it.
class BackendOps {
public:
    virtual ~BackendOps() = default;

    // Populate model-specific metadata (context window, capability labels, …)
    // for a downloaded model. Default: nothing.
    virtual void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const {
        (void)info;
        (void)ctx;
    }

    // Resolve a checkpoint to its absolute on-disk path (file or directory).
    // Default: the shared HF behavior — locate the variant/aux file in the active
    // snapshot, else fall back to the model cache directory. Backends with a
    // bespoke artifact layout (GGUF file, genai_config.json dir, .bin, …) override.
    virtual std::string resolve_checkpoint_path(const ModelInfo& info,
                                                const CheckpointResolveContext& ctx) const;
};

// Shared default ops instance for backends that override nothing.
const BackendOps* default_backend_ops();

} // namespace backends
} // namespace lemon
