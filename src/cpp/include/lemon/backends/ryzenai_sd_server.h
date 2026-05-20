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

class SDNPUServer : public WrappedServer, public IImageServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
            "sd-npu",
    #ifdef _WIN32
            "ryzenai-sd-server.exe"
    #else
            "ryzenai-sd-server"
    #endif
        , get_install_params
    );

    explicit SDNPUServer(const std::string& log_level,
                         ModelManager* model_manager,
                         BackendManager* backend_manager);

    ~SDNPUServer() override;

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

private:
    // Resolve a size string ("WxH") from request, falling back to image_defaults_
    std::string resolve_size(const nlohmann::json& request) const;
    ImageDefaults image_defaults_;
};

} // namespace backends
} // namespace lemon
