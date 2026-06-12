#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class MoonshineServer : public WrappedServer, public ITranscriptionServer, public IStreamingTranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "moonshine",
        "moonshine-server",
        get_install_params
    );

    explicit MoonshineServer(const std::string& log_level,
                            ModelManager* model_manager,
                            BackendManager* backend_manager);

    ~MoonshineServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // ITranscriptionServer implementation
    json audio_transcriptions(const json& request) override;

    // IStreamingTranscriptionServer implementation
    std::string get_streaming_address() override;

private:
    // Forward audio data directly (no file I/O) using multipart form-data
    json forward_multipart_audio_data(const std::string& audio_data,
                                      const std::string& filename,
                                      const json& params);

    int tcp_port_ = 0;     // Port for line-delimited JSON streaming
};

} // namespace backends
} // namespace lemon
