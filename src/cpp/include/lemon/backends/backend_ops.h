#pragma once

#include <string>
#include <vector>
#include "lemon/model_manager.h"  // ModelInfo, DownloadProgressCallback (server-side only)

namespace lemon {

class CloudProviderRegistry;

namespace backends {

// Context handed to BackendOps methods — the bits of server state model
// management needs without a running subprocess. Grows as migrations require.
struct BackendOpsContext {
    ModelManager* model_manager = nullptr;
    CloudProviderRegistry* cloud_registry = nullptr;  // for dynamic cloud discovery
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

    // Find the primary checkpoint artifact inside a freshly-imported local
    // directory (a local_import pull), e.g. the .gguf / .bin file or the
    // genai_config.json directory. Returns the absolute path to register, or ""
    // to register the directory itself. Default: "" (register the directory).
    virtual std::string find_imported_checkpoint(const std::string& import_dir) const {
        (void)import_dir;
        return "";
    }

    // Models supplied at runtime rather than from server_models.json (descriptor
    // dynamic_models = true). Default: none. cloud/flm override.
    virtual std::vector<ModelInfo> discover_models(const BackendOpsContext& ctx) const {
        (void)ctx;
        return {};
    }

    // Whether a model's local artifacts are present. Default: the shared HF
    // checkpoint-completeness check (ModelManager::checkpoints_complete). cloud
    // (always true) and flm (installed-set membership) override.
    virtual bool is_downloaded(const ModelInfo& info, const BackendOpsContext& ctx) const;

    // Validate a resolved checkpoint file for the cache. Returns "" if valid, or
    // a reason it should be treated as not-downloaded. Default: always valid;
    // llamacpp checks GGUF magic.
    virtual std::string validate_checkpoint_file(const std::string& resolved_path) const {
        (void)resolved_path;
        return "";
    }

    // Download a model's artifacts. Default: the shared Hugging Face download.
    // cloud (no-op) and flm (flm pull) override.
    virtual void download_model(const ModelInfo& info, bool do_not_upgrade,
                                DownloadProgressCallback progress,
                                const BackendOpsContext& ctx) const;

    // Whether the model cache must be rebuilt after this backend downloads a
    // model (e.g. flm, whose model list changes). Default: false.
    virtual bool invalidates_cache_after_download() const { return false; }

    // Resolve a backend's installed version for a given backend variant. The
    // caller passes the version read from the on-disk version.txt (or "" if
    // absent); the default returns it unchanged. Backends that detect their
    // version another way override: llamacpp's "system" build runs
    // `llama-server --version`; flm queries `flm version` when no file is present.
    virtual std::string resolve_version(const std::string& backend,
                                        const std::string& file_version) const {
        (void)backend;
        return file_version;
    }

    // Result of a backend-specific install check: whether the backend variant is
    // usable, plus an optional error explaining why not.
    struct InstallCheck {
        bool installed = false;
        std::string error;
    };

    // Decide whether a backend variant is installed, given whether its managed
    // binary was found on disk. Default: installed iff the binary was found.
    // llamacpp's "system" build also requires the ggml HIP plugin when an AMD GPU
    // is present; flm can be a system PATH package even without a managed binary.
    virtual InstallCheck check_install(const std::string& backend, bool binary_found) const {
        (void)backend;
        return {binary_found, ""};
    }
};

// Shared default ops instance for backends that override nothing.
const BackendOps* default_backend_ops();

} // namespace backends
} // namespace lemon
