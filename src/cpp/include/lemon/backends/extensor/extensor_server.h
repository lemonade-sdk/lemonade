#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

class ExtensorServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend,
                                            const std::string& version);

    ExtensorServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);
    ~ExtensorServer() override;

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
    std::string backend_model_id_;
};

namespace extensor {
constexpr uint32_t capabilities() { return capability_mask_of<ExtensorServer>(); }
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace extensor
}  // namespace backends
}  // namespace lemon
