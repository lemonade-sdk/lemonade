#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>
#include <vector>

namespace lemon {
namespace backends {

class AmicaServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit AmicaServer(const std::string& log_level,
                         ModelManager* model_manager,
                         BackendManager* backend_manager);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
};

namespace amica {
// Factory for the amica backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AmicaServer>(); }

// amica-engine, built beside lemond when Lemonade is configured with
// LEMON_AMICA_ROOT. It keeps the model resident, runs it through amica.dll and
// serves the OpenAI-compatible API Lemonade forwards to.
std::string engine_path();

std::vector<std::string> build_engine_argv(const std::string& model_dir,
                                           const std::string& amica_dir,
                                           const std::string& model_name,
                                           int port,
                                           const std::string& amica_args);
}  // namespace amica
}  // namespace backends
}  // namespace lemon
