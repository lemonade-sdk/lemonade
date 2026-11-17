#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/wrapped_server.h"

#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

class AimServer : public WrappedServer {
public:
    AimServer(const std::string& log_level, ModelManager* model_manager,
              BackendManager* backend_manager);
    ~AimServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
                                   httplib::DataSink& sink, bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

private:
    json to_aim_request(const json& request) const;
    json from_aim_response(json response) const;

    // AIM serves the model under its Hugging Face id, whatever Lemonade calls it.
    std::string served_model_;
};

namespace aim {
struct ModelImage {
    std::string image;  // <repository>@<digest>, as pulled and run
    std::string tag;    // the release the digest came from
};

// A model row's `aim_image`, "<repository>:<tag>@sha256:<digest>". Throws for
// anything else.
ModelImage parse_model_image(const std::string& pinned);

// The environment that has AIM serve the model mounted at
// kContainerModelsDir/<org>/<model> on `port`. `ctx_size` overrides the
// profile's context when positive; `profile` overrides AIM's profile choice
// when not empty.
std::vector<std::pair<std::string, std::string>> build_container_env(int port, int ctx_size,
                                                                     const std::string& profile);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AimServer>(); }
}  // namespace aim

}  // namespace backends
}  // namespace lemon
