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

    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer stubs - unsupported on an image-generation backend
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer
    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;

private:
    ImageDefaults image_defaults_;

    std::string resolve_size(const json& request) const;

    // ryzenai-sd-server reads only prompt/size/n from the request body, so
    // steps/cfg_scale/seed/strength reach it through a flat JSON object
    // embedded in the prompt as <sd_cpp_extra_args>...</sd_cpp_extra_args>.
    // Precedence: request -> model image_defaults -> recipe options.
    json build_extra_args(const json& request) const;
};

namespace ryzenaisd {
// Factory for the ryzenaisd backend (lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RyzenAISDServer>(); }
}  // namespace ryzenaisd

}  // namespace backends
}  // namespace lemon
