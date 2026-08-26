#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/utils/process_manager.h"
#include "lemon/backends/backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class SDServer : public WrappedServer, public IImageServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


    explicit SDServer(const std::string& log_level,
                      ModelManager* model_manager,
                      BackendManager* backend_manager);

    ~SDServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer implementation
    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;

    // ESRGAN upscaling via sd-cli subprocess.
    //
    // sd-server's HTTP API does not expose an upscaling endpoint, so we use the
    // sd-cli binary's -M upscale mode as a subprocess.
    static std::string upscale_via_cli(
        const std::string& b64_image,
        const std::string& upscale_model_path);

private:
    // Precedence and fall-through are documented in build_extra_args().
    // `include_flow_shift` is true for /v1/images/generations and
    // /v1/images/edits; false for /v1/images/variations.
    nlohmann::json build_extra_args(const nlohmann::json& request,
                                    bool include_flow_shift = true) const;

    // Resolve the final size string for sd-server. sd-server only reads the
    // OpenAI-style `size: "WxH"` field -- top-level width/height are ignored.
    // Returns "" if no size can be resolved.
    std::string resolve_size(const nlohmann::json& request) const;
};

namespace sdcpp {
// Factory for the sdcpp backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<SDServer>(); }
}  // namespace sdcpp
}  // namespace backends
}  // namespace lemon
