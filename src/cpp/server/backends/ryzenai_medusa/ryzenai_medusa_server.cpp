#include "lemon/backends/ryzenai_medusa/ryzenai_medusa_server.h"
#include "lemon/backends/ryzenai_medusa/ryzenai_medusa.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/utils/path_utils.h"
#include <filesystem>

namespace lemon {
namespace backends {
namespace ryzenai_medusa {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    auto server = std::make_unique<::lemon::RyzenAIServer>(
        ctx.model_info->model_name, ctx.log_level == "debug",
        ctx.model_manager, ctx.backend_manager, spec());
    server->set_model_path(ctx.model_info->resolved_path());
    return server;
}

const BackendSpec* spec() {
    static const BackendSpec kSpec("ryzenai-llm-medusa", descriptor.binary,
                                   ::lemon::RyzenAIServer::get_install_params_medusa, /*split=*/false);
    return &kSpec;
}

namespace {
class RyzenAiMedusaOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        std::string found = find_imported_checkpoint(ctx.model_cache_path);
        return found.empty() ? ctx.model_cache_path : found;
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        std::filesystem::path dir = lemon::utils::path_from_utf8(import_dir);
        if (hf_cache::exists(dir)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(dir, hf_cache::dir_options())) {
                if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                    return lemon::utils::path_to_utf8(entry.path().parent_path());
                }
            }
        }
        return "";
    }
};
}  // namespace

const BackendOps* ops() { return single_ops<RyzenAiMedusaOps>(); }

}  // namespace ryzenai_medusa
}  // namespace backends
}  // namespace lemon
