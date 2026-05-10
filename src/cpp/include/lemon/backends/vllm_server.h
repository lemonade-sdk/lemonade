#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class VLLMServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
            "vllm",
            "vllm-server"
        , get_install_params
        , /*supports_split_archive=*/true
    );

    VLLMServer(const std::string& log_level,
               ModelManager* model_manager,
               BackendManager* backend_manager);

    ~VLLMServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

} // namespace backends
} // namespace lemon
