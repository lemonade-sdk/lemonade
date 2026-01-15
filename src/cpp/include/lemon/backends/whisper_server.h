#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class WhisperServer : public WrappedServer, public IAudioServer {
public:
    explicit WhisperServer(const std::string& log_level = "info",
                          ModelManager* model_manager = nullptr);

    ~WhisperServer() override;

    // WrappedServer interface
    void install(const std::string& backend = "") override;

    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj = "",
                              bool do_not_upgrade = false) override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IAudioServer implementation
    json audio_transcriptions(const json& request) override;

private:
    std::string get_whisper_server_path();
    std::string find_executable_in_install_dir(const std::string& install_dir);
    std::string find_external_whisper_server();

    // Audio file handling
    std::string save_audio_to_temp(const std::string& audio_data,
                                    const std::string& filename);
    void cleanup_temp_file(const std::string& path);
    void validate_audio_file(const std::string& path);

    // Build request for whisper-server
    json build_transcription_request(const json& request, bool translate = false);

    // Forward audio file using multipart form-data
    json forward_multipart_audio_request(const std::string& file_path,
                                         const json& params,
                                         bool translate);

    std::string model_path_;
    std::filesystem::path temp_dir_;  // Directory for temporary audio files
};

} // namespace backends
} // namespace lemon
