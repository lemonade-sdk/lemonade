#pragma once

#include <optional>
#include <string>
#include <vector>
#include "lemon/model_manager.h"  // ModelInfo, DownloadProgressCallback
#include "lemon/models/artifact_fetch.h"  // ArtifactFetchService

namespace lemon {

class CloudProviderRegistry;

namespace models {
struct CheckpointRef;
class ArtifactLayout;
class ModelArtifactStore;
}  // namespace models

namespace backends {

// What a model needs fetched, asked once so the fetch path never branches on a
// backend name: a repository to pull into the models directory, a backend that
// pulls its own weights, or nothing at all. The default answer for a recipe comes
// from its descriptor, which is where a backend already declares itself.
enum class DownloadPolicy : int {
    Repository,
    SelfManaged,
    None,
};

// Context handed to BackendOps methods: the server state model management needs
// without a running subprocess.
struct BackendOpsContext {
    const models::ModelArtifactStore* artifacts = nullptr;
    // Fetching mutates the models directory, so the service is not const.
    models::ArtifactFetchService* fetch = nullptr;
    CloudProviderRegistry* cloud_registry = nullptr;  // for dynamic cloud discovery
};

// Inputs for resolving a checkpoint's on-disk path.
struct CheckpointResolveContext {
    std::string hf_cache;          // HF cache root dir
    std::string model_cache_path;  // hf_cache/<checkpoint repo cache dir>
    std::string repo_id;           // checkpoint's repo id
    std::string main_repo_id;      // the model's "main" checkpoint repo id (fallback)
    std::string variant;           // checkpoint variant after ':' ("" if none)
    std::string type;              // checkpoint type ("main", "mmproj", "npu_cache", …)
    std::string checkpoint;        // the raw checkpoint string
    std::string registry_source;   // huggingface/modelscope
};

// Stateless per-backend behavior for model management that happens WITHOUT a
// running subprocess: checkpoint-path resolution, download, dynamic discovery,
// per-model metadata, version detection, availability. One singleton per
// backend, exposed via lemon::backends::<stem>::ops() and bound in the registry
// (see BackendRegistration::ops). The base class provides shared default
// behavior; backends override only the policy points they need. Every method
// has a default, so adding one never forces edits to backends that don't
// override it.
class BackendOps {
public:
    virtual ~BackendOps() = default;

    // Populate model-specific metadata (context window, capability labels, …)
    // for a downloaded model. Default: nothing.
    virtual void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const {
        (void)info;
        (void)ctx;
    }

    // Add automatic overridable options before exposing/passing to a backend.
    // Managed arguments (model paths/ports) do not belong here. Default: none.
    virtual void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const {
        (void)info;
        (void)options;
    }

    // Resolve a checkpoint to its absolute on-disk path (file or directory).
    // Default: the shared registry-cache behavior — locate the variant/aux file in the active
    // snapshot, else fall back to the model cache directory.
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

// Whether this engine can fetch a checkpoint by registry id for a host speaking
// `protocol`. Default false: lemonade downloads and hands over local paths.
    virtual bool resolves_download_protocol(const std::string& protocol) const {
        (void)protocol;
        return false;
    }

// The ArtifactLayout for one of this backend's checkpoint types; null means the
// snapshot layout.
    virtual const models::ArtifactLayout* checkpoint_layout(
        const models::CheckpointRef& checkpoint) const {
        (void)checkpoint;
        return nullptr;
    }

    virtual bool acquires_lazily(const std::string& checkpoint_type) const {
        (void)checkpoint_type;
        return false;
    }

    // Validate a user-supplied checkpoint string when registering a new model.
    // Return an error message if invalid, "" if acceptable. Default: accept.
    virtual std::string validate_registration_checkpoint(const std::string& checkpoint) const {
        (void)checkpoint;
        return "";
    }

    // Select the repo-relative files to download for the main checkpoint
    // `main_variant`, for backends whose artifact layout isn't a GGUF file.
    // Return nullopt to use the default GGUF selection. (Direct single-file
    // variants — .safetensors/.pth/.ckpt — are handled generically upstream.)
    virtual std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const {
        (void)main_variant;
        (void)repo_files;
        return std::nullopt;
    }

    // Models supplied at runtime rather than from server_models.json (descriptor
    // dynamic_models = true). Default: none.
    virtual std::vector<ModelInfo> discover_models(const BackendOpsContext& ctx) const {
        (void)ctx;
        return {};
    }

    // Whether a model's local artifacts are present. Default: the artifact
    // store's checkpoint-completeness check.
    virtual bool is_downloaded(const ModelInfo& info, const BackendOpsContext& ctx) const;

    virtual DownloadPolicy plan_download(const ModelInfo& info,
                                         const BackendOpsContext& ctx) const;

    // Validate a resolved checkpoint file for the cache. Returns "" if valid, or
    // a reason it should be treated as not-downloaded. Default: always valid.
    virtual std::string validate_checkpoint_file(const std::string& resolved_path) const {
        (void)resolved_path;
        return "";
    }

    // Download a model's artifacts. Default: the shared remote-registry download.
    virtual void download_model(const ModelInfo& info, bool do_not_upgrade,
                                DownloadProgressCallback progress,
                                const BackendOpsContext& ctx) const;

    // Remove weights this backend fetched for itself, as its descriptor's
    // self_manages_downloads promises. Default: nothing to do, because lemond
    // wrote the bytes and its own cache removal reaches them.
    virtual void remove_self_managed_model(const ModelInfo& info) const {
        (void)info;
    }

    // Whether the model cache must be rebuilt after this backend downloads a
    // model. Default: false.
    virtual bool invalidates_cache_after_download() const { return false; }

    // Resolve a backend's installed version for a given backend variant. The
    // caller passes the version read from the on-disk version.txt (or "" if
    // absent); the default returns it unchanged.
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
    virtual InstallCheck check_install(const std::string& backend, bool binary_found) const {
        (void)backend;
        return {binary_found, ""};
    }

    // The /system-info state for a backend variant that is supported but not
    // currently available (install probe failed).
    struct UnavailableState {
        std::string state;    // "installable" | "update_required" | "action_required"
        std::string message;  // shown to the user
        std::string action;   // remediation (a URL or an install command)
        bool attach_installed_version = false;  // surface the installed version too
    };

    // Classify a "supported but not available" backend variant for /system-info,
    // given the install probe's error text and the generic install command the
    // caller would otherwise use. Return nullopt to use the generic
    // installable/no-fetch default.
    virtual std::optional<UnavailableState> classify_unavailable(
        const std::string& backend, const std::string& install_error,
        const std::string& default_install_command) const {
        (void)backend;
        (void)install_error;
        (void)default_install_command;
        return std::nullopt;
    }
};

// Shared default ops instance for backends that override nothing.
const BackendOps* default_backend_ops();

} // namespace backends
} // namespace lemon
