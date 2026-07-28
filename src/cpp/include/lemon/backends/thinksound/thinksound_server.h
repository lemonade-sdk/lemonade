#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <httplib.h>
#include <string>

namespace lemon {
namespace backends {

// ThinkSound SFX backend. Wraps the resident ts-server; maps the
// /audio/generations request onto its POST /generate.
class ThinkSoundServer : public WrappedServer, public IAudioGenerationServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    ThinkSoundServer(const std::string& log_level,
                     ModelManager* model_manager,
                     BackendManager* backend_manager);
    ~ThinkSoundServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade) override;
    void unload() override;

    // IAudioGenerationServer
    void audio_generations(const json& request, httplib::DataSink& sink) override;

private:
    std::string resolve_binary_path(const std::string& backend);
};

namespace thinksound {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace thinksound

}  // namespace backends
}  // namespace lemon
