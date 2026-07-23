#pragma once

#include "../wrapped_server.h"
#include "../server_capabilities.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// Runs the AMD GPU (hipep/AMDGPU EP) Whisper ONNX pipeline as a Python
// subprocess (whisper_server.py in a bundled venv) and forwards audio
// transcription requests to it. Text/completion are unsupported.
class WhisperAMDGPUServer : public WrappedServer, public ITranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "amdgpu-whisper",
        "whisper_server.py",
        get_install_params
    );

    explicit WhisperAMDGPUServer(const std::string& log_level,
                                 ModelManager* model_manager,
                                 BackendManager* backend_manager);

    ~WhisperAMDGPUServer() override;

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

private:
    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
