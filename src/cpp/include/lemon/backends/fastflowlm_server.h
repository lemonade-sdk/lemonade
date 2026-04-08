#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer, public IAudioServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        // recipe
            "flm",
        // executable
    #ifdef _WIN32
            "flm.exe"
    #else
            "flm"
    #endif
        , get_install_params
    );

    FastFlowLMServer(const std::string& log_level, ModelManager* model_manager = nullptr,
                     BackendManager* backend_manager = nullptr);

    ~FastFlowLMServer() override;

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

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;

    // IAudioServer implementation
    json audio_transcriptions(const json& request) override;

    // FLM uses /api/tags for readiness check instead of /health
    bool wait_for_ready();

    // Override to transform model name to checkpoint for FLM
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0) override;

private:
    // Get the path to the flm executable from the install directory
    std::string get_flm_path();

    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
