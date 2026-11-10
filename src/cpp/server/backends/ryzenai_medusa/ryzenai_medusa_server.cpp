#include "lemon/backends/ryzenai_medusa/ryzenai_medusa_server.h"
#include "lemon/backends/ryzenai_medusa/ryzenai_medusa.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"

namespace lemon {
namespace backends {
namespace ryzenai_medusa {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return std::make_unique<::lemon::RyzenAIServer>(ctx.log_level == "debug", ctx.model_manager,
                                                   ctx.backend_manager, descriptor.recipe, spec());
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary,
                                   ::lemon::RyzenAIServer::get_install_params_medusa, /*split=*/false);
    return &kSpec;
}

// Medusa models use the same genai_config.json layout as ryzenai-llm.
const BackendOps* ops() { return ryzenai::ops(); }

}  // namespace ryzenai_medusa
}  // namespace backends
}  // namespace lemon
