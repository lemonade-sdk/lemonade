#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon::backends {

class StrataServer : public WrappedServer {
public:
    StrataServer(const std::string& log_level, ModelManager* model_manager,
                 BackendManager* backend_manager)
        : WrappedServer("Strata", log_level, model_manager, backend_manager) {}
    ~StrataServer() override;
    static InstallParams get_install_params(const std::string& backend, const std::string& version);
    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    json chat_completion(const json& request) override;
    json responses(const json& request) override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
                                   httplib::DataSink& sink, bool sse = true, long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;
    std::string get_address() const override { return ""; }
    void unload() override;

private:
    json native_request(const json& request, bool responses);
};

namespace strata {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<StrataServer>(); }
}

}
