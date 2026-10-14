#pragma once

#include "lemon/backends/configured/backend_transport.h"
#include "lemon/backends/configured/configured_backend_entry.h"
#include "lemon/backends/configured/launch_planner.h"

namespace lemon::backends {

struct ConfiguredServerServices {
    std::shared_ptr<LaunchServices> launch;
    std::function<std::unique_ptr<ServerProcess>(ProcessOutput, const std::string&)> make_process;
};
class ConfiguredServer final : public WrappedServer, public IAudioGenerationServer {
public:
    ConfiguredServer(std::shared_ptr<const ConfiguredBackendEntry> entry, const BackendContext& context,
                     ConfiguredServerServices services = {});
    void load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade = false) override;
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
    void audio_generations(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override;
    AudioFormatMetadata audio_format_metadata(const std::string& format) const override;
    void forward_streaming_request(const std::string& endpoint, const std::string& request_body,
        httplib::DataSink& sink, bool sse = true, long timeout_seconds = 0, TelemetryCallback telemetry_callback = nullptr) override;
private:
    const OperationProgram* program(BackendOperation operation, OperationDelivery delivery) const;
    json send_completion(BackendOperation operation, const json& request);
    const std::shared_ptr<const ConfiguredBackendEntry> entry_;
    ConfiguredServerServices services_;
    MappingContext mapping_;
    const BackendTransport transport_;
};

}
