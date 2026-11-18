#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/vllm/vllm_server.h"
#include "lemon/server_process.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

namespace aim {

// The engines AIM images run, as a model row's `aim_engine` names them.
// Each has its own server class, which inherits the native backend for that
// engine so AIM keeps its request handling: AimVllmServer : VLLMServer now,
// and for example AimLlamaServer : LlamaCppServer later. Only chat engines fit:
// supported modes and the capability mask belong to the recipe, so an engine
// serving another mode, such as AIM's Whisper images, needs its own recipe.
inline constexpr const char* kEngineVllm = "vllm";

// The engine a model row names, "vllm" when it names none.
std::string model_engine(const ModelInfo& info);
bool supports_engine(const std::string& engine);

// Everything about starting an AIM container that does not depend on its
// engine. The engine's server class supplies `engine_args` in its own flag
// names and starts the container with start_server().
struct Launch {
    ServerCommand command;
    std::unique_ptr<ContainerProcess> process;
    long startup_timeout_seconds = 0;
    // The Hugging Face id AIM serves the model under, whatever Lemonade calls it.
    std::string served_model;
};

// Installs the base image, pulls the model's image and builds the container
// that serves `model_name` on `port`. Throws when the model names no valid
// image or its weights are not downloaded.
Launch prepare_launch(const std::string& model_name, const ModelInfo& model_info,
                      const RecipeOptions& options, ModelManager* model_manager,
                      BackendManager* backend_manager, int port, const json& engine_args,
                      bool inherit_output);

struct ModelImage {
    std::string image;  // <repository>@<digest>, as pulled and run
    std::string tag;    // the release the digest came from
};

// A model row's `aim_image`, "<repository>:<tag>@sha256:<digest>". Throws for
// anything else.
ModelImage parse_model_image(const std::string& pinned);

// The environment that has AIM serve the model mounted at
// kContainerModelsDir/<org>/<model> on `port`. `engine_args` override the
// profile's engine arguments; `profile` overrides AIM's profile choice when
// not empty.
std::vector<std::pair<std::string, std::string>> build_container_env(int port,
                                                                     const json& engine_args,
                                                                     const std::string& profile);

// Puts a reply, whole or one stream event, under Lemonade's model name.
void restore_model_name(json& reply, const std::string& model_name);
}  // namespace aim

// An AIM image whose engine is vLLM. VLLMServer supplies the request handling;
// this class starts the container and translates what vLLM in AIM returns.
class AimVllmServer : public VLLMServer {
public:
    AimVllmServer(const std::string& log_level, ModelManager* model_manager,
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
    std::string served_model_;
};

namespace aim {
// Puts a vLLM reply, whole or one stream event, in the terms Lemonade's
// clients read: Lemonade's model name, and vLLM's `reasoning` as
// `reasoning_content`.
void translate_vllm_reply(json& reply, const std::string& model_name);

// Creates the server class for the model's engine. Throws for an engine
// Lemonade does not run.
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
// Every engine's server class must serve the same capabilities.
constexpr uint32_t capabilities() { return capability_mask_of<AimVllmServer>(); }
}  // namespace aim

}  // namespace backends
}  // namespace lemon
