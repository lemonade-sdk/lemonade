#pragma once

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lemon {

class ExternalBackendServer : public WrappedServer,
                               public IEmbeddingsServer,
                               public IRerankingServer,
                               public ITranscriptionServer,
                               public IStreamingTranscriptionServer,
                               public ITextToSpeechServer,
                               public IClassificationServer,
                               public IImageServer,
                               public IAudioGenerationServer,
                               public IModel3DServer,
                               public ISlotsServer,
                               public ITokenizerServer {
public:
    ExternalBackendServer(const std::string& server_name);
    virtual ~ExternalBackendServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;
    void unload() override;

    bool downsize() override;
    void restore() override;

    bool wait_for_ready(const std::string& endpoint, long timeout_seconds = 600, long poll_interval_ms = 100) override;

    bool has_capability(const std::string& cap_name) const override;
    bool supports_downsize() const override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    json embeddings(const json& request) override;

    json reranking(const json& request) override;

    json audio_transcriptions(const json& request) override;

    std::string get_streaming_address() override;

    void audio_speech(const json& request, httplib::DataSink& sink) override;

    json classify(const json& request) override;

    json image_generations(const json& request) override;
    json image_edits(const json& request) override;
    json image_variations(const json& request) override;

    void audio_generations(const json& request, httplib::DataSink& sink) override;

    void model_3d_generations(const json& request, httplib::DataSink& sink) override;

    json get_slots() override;
    json slots_action(int slot_id, const std::string& action, const json& request_body) override;

    json tokenize(const json& request_body) override;

private:
    std::vector<std::string> resolve_command_args(
        const std::vector<std::string>& template_args,
        const std::unordered_map<std::string, std::string>& token_map,
        const RecipeOptions& recipe_options);

    bool perform_health_probe(const std::string& endpoint,
                              int expected_status,
                              int timeout_seconds,
                              int poll_interval_ms);

    std::string selected_platform_;
    BackendDescriptor::PlatformConfig active_platform_config_;
    std::unordered_map<std::string, std::string> instance_token_map_;
    RecipeOptions loaded_recipe_options_;
};

} // namespace lemon
