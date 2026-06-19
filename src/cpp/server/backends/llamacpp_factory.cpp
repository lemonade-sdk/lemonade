#include "lemon/backends/llamacpp_factory.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> llamacpp_create(const BackendContext& ctx) {
    return std::make_unique<LlamaCppServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
