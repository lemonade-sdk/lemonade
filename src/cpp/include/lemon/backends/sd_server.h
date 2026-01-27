#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "../recipe_options.h"
#include "../utils/process_manager.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class SDServer : public WrappedServer, public IImageServer {
public:
    explicit SDServer(const std::string& log_level = "info",
                      ModelManager* model_manager = nullptr);

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
    // Server executable helper
    std::string find_executable_in_install_dir(const std::string& install_dir);

    // Server lifecycle helpers
    bool wait_for_ready(int timeout_seconds = 60) override;

    // Server state (port_ and process_handle_ inherited from WrappedServer)
    std::string model_path_;
};

} // namespace backends
} // namespace lemon
