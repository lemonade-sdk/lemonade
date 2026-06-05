#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include "flm_engine.h"
#include <string>

namespace lemon {
namespace backends {

// FLM types live in lemonade::backends
using lemonade::backends::FlmEngine;
using lemonade::backends::FlmInferenceResult;
using lemonade::backends::FlmModelConfig;
using lemonade::backends::FlmStreamCallback;

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer,
                         public IRerankingServer, public ITranscriptionServer {
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

    // ITranscriptionServer implementation
    json audio_transcriptions(const json& request) override;

    // Override to transform model name to checkpoint for FLM
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0) override;

private:
    // Get the path to the flm executable for model pulling
    std::string get_flm_path();

    // Build an OpenAI-style chat completion response from FlmEngine result
    json build_chat_response(const std::string& model,
                             const FlmInferenceResult& result,
                             int prompt_tokens,
                             int completion_tokens);

    // Build an OpenAI-style completion response
    json build_completion_response(const std::string& model,
                                   const std::string& text,
                                   int prompt_tokens,
                                   int completion_tokens);

    // Streaming chat completion using FlmEngine directly
    void stream_chat_completion(const json& request,
                                httplib::DataSink& sink);

    // Streaming text completion using FlmEngine directly
    void stream_completion(const json& request,
                           httplib::DataSink& sink);

    bool is_loaded_ = false;
    std::unique_ptr<FlmEngine> engine_;
    std::string current_model_tag_;
};

} // namespace backends
} // namespace lemon
