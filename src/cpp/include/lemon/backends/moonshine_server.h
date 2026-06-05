#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class MoonshineServer : public WrappedServer, public ITranscriptionServer, public IStreamingTranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "moonshine",
        "moonshine-server"
#ifndef _WIN32
        ".py"
#endif
        , get_install_params
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
    // Build request for moonshine-server
    json build_transcription_request(const json& request);

    // Forward audio file using multipart form-data
    json forward_multipart_audio_request(const std::string& file_path,
                                         const json& params);

    // Forward audio data directly (no file I/O) using multipart form-data
    json forward_multipart_audio_data(const std::string& audio_data,
                                      const std::string& filename,
                                      const json& params);

    std::string model_path_;
    int model_arch_ = 5;  // Default: MEDIUM_STREAMING
    int tcp_port_ = 0;     // Port for line-delimited JSON streaming
};

} // namespace backends
} // namespace lemon
