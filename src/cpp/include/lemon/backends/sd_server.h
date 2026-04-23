#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "../model_manager.h"
#include "../recipe_options.h"
#include "../utils/process_manager.h"
#include "backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class SDServer : public WrappedServer, public IImageServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
            "sd-cpp",
    #ifdef _WIN32
            "sd-server.exe"
    #else
            "sd-server"
    #endif
        , get_install_params
    );

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
    //
    // Called by Server::handle_image_upscale (server.cpp), which is registered
    // as the route handler for POST /api/v1/images/upscale (see register_post
    // in Server::Server).
    //
    // Endpoint: POST /api/v1/images/upscale
    //   Request body (JSON):
    //     { "image": "<base64 PNG>", "model": "<model name, e.g. RealESRGAN-x4plus>" }
    //   Success response (200):
    //     { "created": <timestamp>, "data": [{ "b64_json": "<base64 PNG>" }] }
    //   Error responses:
    //     400 - missing "image" or "model" field
    //     404 - model name not found in server_models.json
    //     500 - upscale subprocess failed or sd-cli binary not found
    static std::string upscale_via_cli(
        const std::string& b64_image,
        const std::string& upscale_model_path,
        const std::string& cli_exe_path,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        bool debug = false);

private:
    // image_defaults from the currently loaded model's server_models.json entry.
    // Applied when a request doesn't specify size / steps / cfg_scale / etc.
    // Needed because sd-server's own defaults are fixed at process startup and
    // OmniRouter tool calls arrive without these fields.
    ImageDefaults image_defaults_;

    // Build the <sd_cpp_extra_args> JSON. Precedence: request -> image_defaults_
    // -> recipe_options_. `include_flow_shift` is true for /v1/images/generations
    // and /v1/images/edits; false for /v1/images/variations (which strips prompt).
    nlohmann::json build_extra_args(const nlohmann::json& request,
                                    bool include_flow_shift = true) const;

    // Resolve the final size string for sd-server. sd-server only reads the
    // OpenAI-style `size: "WxH"` field -- top-level width/height are ignored.
    // Returns "" if no size can be resolved.
    std::string resolve_size(const nlohmann::json& request) const;
};

} // namespace backends
} // namespace lemon
