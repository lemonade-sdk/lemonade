#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

class MlxServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    MlxServer(const std::string& log_level,
              ModelManager* model_manager,
              BackendManager* backend_manager);

    ~MlxServer() override;

    DeviceType effective_device(const RecipeOptions& options) const override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

private:
    json prepare_request(const json& request) const;
    bool restart_backend_after_cancel();
    bool ensure_backend_ready();

    std::string loaded_model_ref_;
    std::string launch_executable_;
    std::vector<std::string> launch_args_;
    std::vector<std::pair<std::string, std::string>> launch_env_vars_;
    bool launch_inherit_output_ = false;
    std::mutex backend_restart_mutex_;
};

namespace mlx {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace mlx

}  // namespace backends
}  // namespace lemon
