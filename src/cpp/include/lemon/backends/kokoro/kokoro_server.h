#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class KokoroServer : public WrappedServer, public ITextToSpeechServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


    explicit KokoroServer(const std::string& log_level,
                          ModelManager* model_manager,
                          BackendManager* backend_manager);

    ~KokoroServer() override;

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

namespace kokoro {
// Factory for the kokoro backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace kokoro
}  // namespace backends
}  // namespace lemon
