#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class LlamaCppServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer,
                       public IClassificationServer, public ISystemOneServer,
                       public ISlotsServer, public ITokenizerServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


    LlamaCppServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);

    ~LlamaCppServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;

    // IClassificationServer implementation: zero-shot labels, answered through
    // /v1/systemone. Only a model labelled `systemone` serves it.
    json classify(const json& request) override;

    // ISystemOneServer implementation
    nlohmann::ordered_json systemone(const nlohmann::ordered_json& request) override;

    // ISlotsServer implementation
    json get_slots() override;
    json slots_action(int slot_id, const std::string& action, const json& request_body) override;

    // ITokenizerServer implementation
    json tokenize(const json& request) override;

private:
    // llama-server echoes the local .gguf path it was launched with (`-m <path>`)
    // in the OpenAI `model` field. Rewrite it to the client-facing model id so
    // responses don't leak absolute filesystem paths (and usernames).
    json normalize_response_model(json response, const json& request) const;

    json unsupported_decision_request(const std::string& operation) const;
    nlohmann::ordered_json forward_systemone(const nlohmann::ordered_json& body);

    bool decision_model_ = false;
    int encoder_batch_ctx_ = 0;
};

namespace llamacpp {
// Factory for the llamacpp backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<LlamaCppServer>(); }
}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
