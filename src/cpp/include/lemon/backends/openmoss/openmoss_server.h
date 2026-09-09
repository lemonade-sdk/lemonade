#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/backends/backend_utils.h"
#include "lemon/server_capabilities.h"
#include "lemon/wrapped_server.h"
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
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
    bool is_backend_alive() const override;

    void audio_speech(const json& request, httplib::DataSink& sink) override;
    void audio_generations(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override {
        return {"wav", "pcm"};
    }
    std::vector<std::string> supported_streaming_audio_formats() const override {
        return {"pcm"};
    }
    AudioFormatMetadata audio_format_metadata(const std::string& response_format) const override;

private:
    struct Subprocess {
        ProcessHandle handle;
        int port = 0;
        std::vector<std::string> args;
    };

    std::string resolve_binary_path(const std::string& backend);
    Subprocess spawn(const std::string& model_path);

    void stop_speech_process();
    void start_speech_process(long timeout_seconds = 600);

    std::string design_reference_sample(const std::string& voice_description,
                                        httplib::DataSink& sink);
    std::string render_reference_sample(
        const std::string& voice_description, httplib::DataSink& sink,
        std::chrono::steady_clock::time_point deadline);
    json apply_voice_design(const json& request, httplib::DataSink& sink);

    std::string exe_path_;
    std::string model_path_;
    std::string voicegen_path_;
    std::map<std::string, std::string> reference_cache_;
    std::vector<std::pair<std::string, std::string>> env_vars_;
    std::shared_mutex request_mutex_;
    std::atomic<bool> process_swap_in_progress_{false};
    std::atomic<int> pcm_sample_rate_{0};
    std::atomic<int> pcm_channels_{0};
};

namespace openmoss {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<OpenMossServer>(); }
}  // namespace openmoss

}  // namespace backends
}  // namespace lemon
