#include "lemon/backends/fastflowlm_factory.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> fastflowlm_create(const BackendContext& ctx) {
    return std::make_unique<FastFlowLMServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
