#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/model_manager_types.h"
#include "lemon/recipe_options.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class TheNoiseServer : public WrappedServer, public IImageServer, public IUpscaleServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit TheNoiseServer(const std::string& log_level,
                            ModelManager* model_manager,
                            BackendManager* backend_manager);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer implementation
    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;

    std::string upscale_via_cli(
        const std::string& b64_image,
        const std::string& upscale_model_path,
        double upscale_factor) override;

private:
    nlohmann::json build_request(const nlohmann::json& request) const;

    // Returns "" when no size is named anywhere.
    std::string resolve_size(const nlohmann::json& request) const;
};

namespace thenoise {
// Factory for the thenoise backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<TheNoiseServer>(); }
}  // namespace thenoise
}  // namespace backends
}  // namespace lemon
