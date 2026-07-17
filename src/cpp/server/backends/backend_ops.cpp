#include "lemon/backends/backend_ops.h"

#include <algorithm>
#include <filesystem>
#include "lemon/backends/hf_cache_util.h"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

// Default checkpoint resolution: the shared remote-registry cache behavior. Locate the
// requested variant (or auxiliary file like mmproj) within the active snapshot,
// falling back to the main repo and finally the model cache directory.
std::string BackendOps::resolve_checkpoint_path(const ModelInfo& info,
                                                const CheckpointResolveContext& ctx) const {
    (void)info;

    // NPU side-cache checkpoints have no resolvable local file here (the backend
    // that uses them resolves them itself at load time).
    if (ctx.type == "npu_cache") {
        return "";
    }

    fs::path model_cache_path_fs = path_from_utf8(ctx.model_cache_path);

    if (!ctx.variant.empty()) {
        // Prefer refs/main for auxiliary checkpoints too (e.g. mmproj) so
        // companion files stay on the active snapshot as the main model.
        fs::path active_snapshot = hf_cache::active_snapshot_path(model_cache_path_fs);
        if (!active_snapshot.empty()) {
            fs::path direct_variant_path = active_snapshot / path_from_utf8(ctx.variant);
            if (hf_cache::exists(direct_variant_path)) {
                return path_to_utf8(direct_variant_path);
            }
            std::error_code ec;
            for (const auto& entry :
                 fs::recursive_directory_iterator(active_snapshot, hf_cache::dir_options(), ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec)) {
                    if (entry.path().filename().string() == ctx.variant) {
                        return path_to_utf8(entry.path());
                    }
                } else if (entry.is_directory(ec)) {
                    fs::path variant_path = entry.path() / path_from_utf8(ctx.variant);
                    if (hf_cache::exists(variant_path)) {
                        return path_to_utf8(variant_path);
                    }
                }
                ec.clear();
            }
        }

        // Try to find the exact variant in the cache directory's subtree.
        if (hf_cache::exists(model_cache_path_fs)) {
            for (const auto& entry :
                 fs::recursive_directory_iterator(model_cache_path_fs, hf_cache::dir_options())) {
                if (entry.is_regular_file()) {
                    if (entry.path().filename().string() == ctx.variant) {
                        return path_to_utf8(entry.path());
                    }
                } else if (entry.is_directory()) {
                    fs::path variant_path = entry.path() / path_from_utf8(ctx.variant);
                    if (hf_cache::exists(variant_path)) {
                        return path_to_utf8(variant_path);
                    }
                }
            }
        }

        // Backward-compat: older downloads placed all files in the main repo dir.
        if (ctx.repo_id != ctx.main_repo_id) {
            std::string main_cache_path =
                ctx.hf_cache + "/" + hf_cache::repo_id_to_cache_dir_name(ctx.main_repo_id, ctx.registry_source);
            fs::path main_cache_path_fs = path_from_utf8(main_cache_path);
            if (fs::exists(main_cache_path_fs)) {
                for (const auto& entry : fs::recursive_directory_iterator(main_cache_path_fs)) {
                    if (entry.is_regular_file()) {
                        if (entry.path().filename().string() == ctx.variant) {
                            return path_to_utf8(entry.path());
                        }
                    } else if (entry.is_directory()) {
                        fs::path variant_path = entry.path() / path_from_utf8(ctx.variant);
                        if (fs::exists(variant_path)) {
                            return path_to_utf8(variant_path);
                        }
                    }
                }
            }
        }

        // Variant not found — signal not downloaded.
        return "";
    }

    // No variant: return the cache directory.
    return ctx.model_cache_path;
}

bool BackendOps::is_downloaded(const ModelInfo& info, const BackendOpsContext& ctx) const {
    // Default: the shared registry checkpoint-completeness check.
    return ctx.model_manager != nullptr && ctx.model_manager->checkpoints_complete(info);
}

void BackendOps::download_model(const ModelInfo& info, bool do_not_upgrade,
                                DownloadProgressCallback progress, const BackendOpsContext& ctx) const {
    // Default: the shared remote-registry download engine.
    (void)do_not_upgrade;
    if (ctx.model_manager != nullptr) {
        ctx.model_manager->download_from_registry_engine(info, progress);
    }
}

const BackendOps* default_backend_ops() {
    static const BackendOps kDefault;
    return &kDefault;
}

} // namespace backends
} // namespace lemon
