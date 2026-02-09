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
    inline static const BackendSpec SPEC = BackendSpec(
        // recipe
            "sd-cpp",
        // executable
    #ifdef _WIN32
            "sd-server.exe"
    #else
            "sd-server"
    #endif
    );

    explicit SDServer(const std::string& log_level = "info",
                      ModelManager* model_manager = nullptr,
                      const std::string& backend = "cpu");

    ~SDServer() override;

    // WrappedServer interface
    void install(const std::string& backend = "") override;

    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;

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
  
private:
    // Server state (port_ and process_handle_ inherited from WrappedServer)
    std::string model_path_;
    std::string backend_;
};

} // namespace backends
} // namespace lemon
