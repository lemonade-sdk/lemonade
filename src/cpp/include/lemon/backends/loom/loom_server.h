#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

// Serves a Loom bundle (a model card with its compiled programs and the GGUFs it
// pins) through loom-serve from ROCm/hrx-system.
class LoomServer : public WrappedServer {
public:
    LoomServer(const std::string& log_level, ModelManager* model_manager,
               BackendManager* backend_manager);

    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;

    json chat_completion(const json& request) override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
                                   httplib::DataSink& sink, bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

private:
    void wait_for_bundle(const std::string& model_name);

    std::string served_model_id_;
};

namespace loom {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<LoomServer>(); }
}  // namespace loom

}  // namespace backends
}  // namespace lemon
