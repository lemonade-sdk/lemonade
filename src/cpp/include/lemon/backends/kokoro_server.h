#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class KokoroServer : public WrappedServer, public ITextToSpeechServer {
public:
    explicit KokoroServer(const std::string& log_level,
                          ModelManager* model_manager);

    ~KokoroServer() override;

    // WrappedServer interface
    void install(const std::string& backend) override;

    std::string download_model(const std::string& checkpoint,
                              const std::string& mmproj,
                              bool do_not_upgrade) override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade) override;

    void unload() override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // ITextToSpeechServer implementation
    void audio_speech(const json& request, httplib::DataSink& sink) override;

private:
    std::string get_kokoro_server_path();
    std::string find_executable_in_install_dir(const std::string& install_dir);
    std::string find_external_kokoro_server();

    // Server lifecycle helpers
    bool wait_for_ready(int timeout_seconds = 60);
};

} // namespace backends
} // namespace lemon
