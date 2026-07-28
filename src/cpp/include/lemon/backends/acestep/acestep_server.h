#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <httplib.h>
#include <string>

namespace lemon {
namespace backends {

// ACE-Step music backend. Wraps the resident ace-server, whose synth API is
// asynchronous (submit a job, then poll), so audio_generations() submits and
// polls before streaming the wav bytes back.
class AceStepServer : public WrappedServer, public IAudioGenerationServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    AceStepServer(const std::string& log_level,
                  ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~AceStepServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade) override;
    void unload() override;

    // IAudioGenerationServer
    void audio_generations(const json& request, httplib::DataSink& sink) override;

private:
    std::string resolve_binary_path(const std::string& backend);
    bool run_job(const std::string& path, const std::string& body,
                 std::string& result, std::string& error);
};

namespace acestep {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace acestep

}  // namespace backends
}  // namespace lemon
