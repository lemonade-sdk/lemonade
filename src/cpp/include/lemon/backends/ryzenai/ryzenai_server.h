#pragma once

#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server_capabilities.h"
#include "lemon/wrapped_server.h"

#include <string>

namespace lemon {

using backends::BackendSpec;
using backends::InstallParams;

class RyzenAIServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


    RyzenAIServer(bool debug, ModelManager* model_manager, BackendManager* backend_manager);
    RyzenAIServer(bool debug, ModelManager* model_manager, BackendManager* backend_manager,
                  backends::BackendEffects effects);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // Inference operations (from ICompletionServer via WrappedServer)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
private:
    backends::BackendEffects effects_;
};

} // namespace lemon

namespace lemon {
namespace backends {
namespace ryzenai {
// Factory for the ryzenai backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* legacy_spec();
const BackendOps* legacy_ops();
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RyzenAIServer>(); }
}  // namespace ryzenai
}  // namespace backends
}  // namespace lemon
