#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class KokoroServer : public WrappedServer, public ITextToSpeechServer {
public:
    inline static const BackendSpec SPEC = BackendSpec(
        // recipe
            "kokoro",
        // executable
    #ifdef _WIN32
            "koko.exe"
    #else
            "koko"
    #endif
    );

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
};

} // namespace backends
} // namespace lemon
