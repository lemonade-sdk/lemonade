#pragma once

#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/configured/backend_transport.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/launch_planner.h"

namespace lemon::backends {

struct ExecutionSession;
struct ConfiguredServerServices {
    std::shared_ptr<LaunchServices> launch;
    std::function<std::unique_ptr<ServerProcess>(ProcessOutput, const std::string&)> make_process;
};
class ConfiguredServer final : public WrappedServer, public IEmbeddingsServer, public IRerankingServer,
                               public IClassificationServer, public ITranscriptionServer, public IStreamingTranscriptionServer,
                               public ISlotsServer, public ITokenizerServer, public ITextToSpeechServer,
                               public IAudioGenerationServer, public IModel3DServer {
public:
    ConfiguredServer(std::shared_ptr<const ConfiguredBackendEntry> entry, const BackendContext& context,
                     ConfiguredServerServices services = {});
    void load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade = false) override;
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    json embeddings(const json& request) override;
    json reranking(const json& request) override;
    json get_slots() override;
    json slots_action(int slot_id, const std::string& action, const json& request_body) override;
    json tokenize(const json& request_body) override;
    json classify(const json& request) override;
    json audio_transcriptions(const json& request) override;
    std::string get_streaming_address() override;
    void audio_speech(const json& request, httplib::DataSink& sink) override;
    void audio_generations(const json& request, httplib::DataSink& sink) override;
    void model_3d_generations(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override;
    std::vector<std::string> supported_streaming_audio_formats() const override;
    AudioFormatMetadata audio_format_metadata(const std::string& format) const override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
        httplib::DataSink& sink, bool sse = true, long timeout_seconds = 0, TelemetryCallback telemetry_callback = nullptr) override;
private:
    friend struct ConfiguredServerTestAccess;
    const OperationProgram* program(BackendOperation operation, OperationDelivery delivery) const;
    const OperationProgram& bound(BackendOperation operation) const;
    const OperationProgram* media(OperationDelivery delivery) const;
    OperationHookContext hook_context();
    json send_json(const MappedRequest& mapped, const json& request);
    json send_completion(BackendOperation operation, const json& request);
    json send_v2(BackendOperation operation, const json& request, int slot_id = -1, const std::string& action = "");
    const std::shared_ptr<const ConfiguredBackendEntry> entry_;
    ConfiguredServerServices services_;
    MappingContext mapping_;
    std::shared_ptr<const ExecutionSession> session_;
    const BackendTransport transport_;
};

}
