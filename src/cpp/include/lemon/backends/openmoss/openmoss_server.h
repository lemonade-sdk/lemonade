#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// OpenMOSS (MOSS-TTS-Delay) text-to-speech. Wraps the resident moss-tts-server,
// which already speaks the OpenAI /v1/audio/speech schema, so audio_speech()
// just forwards the request and streams the wav bytes back.
class OpenMossServer : public WrappedServer, public ITextToSpeechServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    OpenMossServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);
    ~OpenMossServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade) override;
    void unload() override;

    // ITextToSpeechServer
    void audio_speech(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override { return {"wav"}; }

private:
    // An explicit "<variant>_bin" config path wins (use a locally-built binary
    // with no published release); otherwise install the managed binary.
    std::string resolve_binary_path(const std::string& backend);
};

namespace openmoss {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace openmoss

}  // namespace backends
}  // namespace lemon
