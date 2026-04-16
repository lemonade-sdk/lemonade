#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class MLXServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        // recipe
            "mlx",
        // executable
            "server"
        , get_install_params
    );

    MLXServer(const std::string& log_level, ModelManager* model_manager = nullptr,
              BackendManager* backend_manager = nullptr);

    ~MLXServer() override;

    std::string download_model(const std::string& checkpoint,
                              bool do_not_upgrade = false);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // MLX engine uses /health for readiness check
    bool wait_for_ready();

    // Override to transform model name to checkpoint for MLX engine
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0) override;

private:
    // Get the path to the mlx-engine server binary
    std::string get_mlx_server_path();

    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
