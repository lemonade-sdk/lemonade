#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>
#include <filesystem>

namespace lemon {
namespace backends {

class ParakeetServer : public WrappedServer, public ITranscriptionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit ParakeetServer(const std::string& log_level,
                            ModelManager* model_manager,
                            BackendManager* backend_manager);

    ~ParakeetServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    json audio_transcriptions(const json& request) override;

private:
    json forward_multipart_audio_data(const std::string& audio_data,
                                      const std::string& filename,
                                      const json& params);

    std::string model_path_;
    std::filesystem::path temp_dir_;
};

namespace parakeet {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace parakeet

}  // namespace backends
}  // namespace lemon
