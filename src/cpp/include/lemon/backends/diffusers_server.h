#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "../model_manager.h"
#include "../recipe_options.h"
#include "../utils/process_manager.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class DiffusersServer : public WrappedServer, public IImageServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
            "diffusers",
            "diffusers-server"
        , get_install_params
        , /*supports_split_archive=*/true
    );

    DiffusersServer(const std::string& log_level,
                    ModelManager* model_manager,
                    BackendManager* backend_manager);

    ~DiffusersServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer (not supported — diffusers is image-only)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IImageServer
    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;
};

} // namespace backends
} // namespace lemon
