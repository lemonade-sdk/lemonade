#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/models/registry_types.h"
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public ITranscriptionServer,
                         public IClassificationServer, public ISystemOneServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    FastFlowLMServer(const std::string& log_level, ModelManager* model_manager = nullptr,
                     BackendManager* backend_manager = nullptr);

    std::string download_model(const std::string& checkpoint,
                              bool do_not_upgrade = false,
                              RegistryId source = models::huggingface_descriptor().id);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // ITranscriptionServer implementation
    json audio_transcriptions(const json& request) override;

    // IClassificationServer implementation: zero-shot labels as one `choice`
    // question to a d3 decision model
    json classify(const json& request) override;

    // ISystemOneServer implementation
    nlohmann::ordered_json systemone(const nlohmann::ordered_json& request) override;

    // Override to transform model name to checkpoint for FLM
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = utils::HttpClient::kNoTimeout,
                                   TelemetryCallback telemetry_callback = nullptr) override;

private:
    bool serves_text() const;
    json unsupported_decision_request(const std::string& operation) const;
    // /v1/systemone for both systemone() and classify(): the window refusal as a
    // 400, else `truncated` on each answer.
    nlohmann::ordered_json decide(const nlohmann::ordered_json& body);

    // Get the path to the flm executable from the install directory
    std::string get_flm_path();
};

namespace fastflowlm {
// Factory for the fastflowlm backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<FastFlowLMServer>(); }
}  // namespace fastflowlm
}  // namespace backends
}  // namespace lemon
