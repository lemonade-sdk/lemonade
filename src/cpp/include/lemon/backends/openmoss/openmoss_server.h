#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

class OpenMossServer : public WrappedServer,
                       public ITextToSpeechServer,
                       public IAudioGenerationServer {
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

    void audio_speech(const json& request, httplib::DataSink& sink) override;
    void audio_generations(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override { return {"wav"}; }

private:
    struct Subprocess {
        ProcessHandle handle;
        int port = 0;
    };

    std::string resolve_binary_path(const std::string& backend);
    Subprocess spawn(const std::string& model_path);

    void stop_speech_process();
    void start_speech_process();

    std::string design_reference_sample(const std::string& voice_description);
    std::string render_reference_sample(const std::string& voice_description);
    json apply_voice_design(const json& request);

    std::string exe_path_;
    std::string model_path_;
    std::string voicegen_path_;
    std::map<std::string, std::string> reference_cache_;
    std::vector<std::pair<std::string, std::string>> env_vars_;
    std::mutex design_mutex_;
};

namespace openmoss {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<OpenMossServer>(); }
}  // namespace openmoss

}  // namespace backends
}  // namespace lemon
