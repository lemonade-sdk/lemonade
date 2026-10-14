#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server_capabilities.h"
#include "lemon/wrapped_server.h"

#include <mutex>
#include <string>

namespace lemon {
namespace backends {

class WhisperServer : public WrappedServer, public ITranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit WhisperServer(const std::string& log_level,
                          ModelManager* model_manager,
                          BackendManager* backend_manager);

    ~WhisperServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // ICompletionServer implementation (not supported - return errors)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // ITranscriptionServer implementation
    json audio_transcriptions(const json& request) override;

private:
    // NPU compiled cache handling
    void download_npu_compiled_cache(const std::string& model_path,
                                      const ModelInfo& model_info,
                                      bool do_not_upgrade);

    // Forward audio data directly (no file I/O) using multipart form-data
    json forward_multipart_audio_data(const std::string& audio_data,
                                      const std::string& filename,
                                      const json& params,
                                      bool translate);

    std::string model_path_;
    std::mutex inference_mutex_;
};

namespace whispercpp {
// Factory for the whispercpp backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<WhisperServer>(); }
}  // namespace whispercpp
}  // namespace backends
}  // namespace lemon
