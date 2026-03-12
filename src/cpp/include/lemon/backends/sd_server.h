#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "../recipe_options.h"
#include "../utils/process_manager.h"
#include "backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class SDServer : public WrappedServer, public IImageServer {
public:
#ifndef LEMONADE_TRAY
    static InstallParams get_install_params(const std::string& backend, const std::string& version);
#endif

    inline static const BackendSpec SPEC = BackendSpec(
            "sd-cpp",
    #ifdef _WIN32
            "sd-server.exe"
    #else
            "sd-server"
    #endif
#ifndef LEMONADE_TRAY
        , get_install_params
#endif
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

    // ESRGAN upscaling via sd-cli subprocess (used by server handler for SSE progress)
    static std::string upscale_via_cli(
        const std::string& b64_image,
        const std::string& upscale_model_path,
        const std::string& cli_exe_path,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        bool debug = false);
};

} // namespace backends
} // namespace lemon
