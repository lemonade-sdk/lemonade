#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/vllm/vllm_server.h"

#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

// AIM runs vLLM, so AimServer keeps VLLMServer's request handling and changes
// only how the server starts and the names AIM uses.
class AimServer : public VLLMServer {
public:
    AimServer(const std::string& log_level, ModelManager* model_manager,
              BackendManager* backend_manager);

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
                                   httplib::DataSink& sink, bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

protected:
    std::string served_model_name() const override { return served_model_; }
    StreamingProxy::ChunkRewriter sse_chunk_rewriter() const override;

private:
    json to_aim_request(const json& request) const;
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

// Puts an AIM reply, whole or one stream event, in the terms Lemonade's
// clients read: `model_name` for the model, and vLLM's `reasoning` as
// `reasoning_content`.
void translate_reply(json& reply, const std::string& model_name);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AimServer>(); }
}  // namespace aim

}  // namespace backends
}  // namespace lemon
