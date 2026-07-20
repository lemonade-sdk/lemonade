#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class RyzenAISDServer : public WrappedServer, public IImageServer {
public:
    explicit RyzenAISDServer(const std::string& log_level,
                             ModelManager* model_manager,
                             BackendManager* backend_manager);

    ~RyzenAISDServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer stubs — not supported for image-generation backends
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer
    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;

private:
    // image_defaults from the currently loaded model's server_models.json entry.
    ImageDefaults image_defaults_;

    // Resolve the final "WxH" size string for the request.
    std::string resolve_size(const json& request) const;
};

namespace ryzenaisd {
// Factory for the ryzenaisd backend (lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace ryzenaisd

}  // namespace backends
}  // namespace lemon
