#pragma once

#include "../server_capabilities.h"
#include "../wrapped_server.h"
#include "backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// Chatterbox text-to-speech backend (Resemble AI). Runs the self-contained
// chatterbox-server subprocess (PyInstaller bundle from the
// lemonade-sdk/chatterbox-rocm distribution repo) and forwards OpenAI-style
// /v1/audio/speech requests to it. Supports CUDA, ROCm, Metal, and CPU; the
// device variant is auto-selected (GPU when available, else CPU) via the
// RECIPE_DEFS preference order in system_info.cpp.
class ChatterboxServer : public WrappedServer, public ITextToSpeechServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "chatterbox",
#ifdef _WIN32
        "chatterbox-server.exe"
#else
        "chatterbox-server"
#endif
        , get_install_params
        , true  // supports_split_archive: GPU bundles exceed GitHub's 2 GiB asset limit
    );

    explicit ChatterboxServer(const std::string& log_level,
                              ModelManager* model_manager,
                              BackendManager* backend_manager);

    ~ChatterboxServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

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
