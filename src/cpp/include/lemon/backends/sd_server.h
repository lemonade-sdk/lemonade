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
    // Server executable helpers
    std::string get_sd_server_path();
    std::string find_executable_in_install_dir(const std::string& install_dir);
    std::string find_external_sd_executable();

    // Server lifecycle helpers
    int choose_port();
    bool wait_for_ready(int timeout_seconds = 60);

    // Server state
    std::string model_path_;
    int port_ = 0;
    utils::ProcessHandle process_handle_;
};

} // namespace backends
} // namespace lemon
