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
    // Kokoros deserializes all six OpenAI formats but only implements four; aac and
    // flac silently fall back to MP3 bytes, so they are not advertised.
    std::vector<std::string> supported_audio_formats() const override {
        return {"mp3", "wav", "opus", "pcm"};
    }
    // Its streaming path returns headerless s16le regardless of response_format
    // ("Force PCM for optimal streaming performance"), so anything else would be
    // served under a Content-Type the bytes don't match.
    std::vector<std::string> supported_streaming_audio_formats() const override {
        return {"pcm"};
    }
};

namespace kokoro {
// Factory for the kokoro backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<KokoroServer>(); }
}  // namespace kokoro
}  // namespace backends
}  // namespace lemon
