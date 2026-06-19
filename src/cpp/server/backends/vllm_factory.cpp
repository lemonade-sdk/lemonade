#include "lemon/backends/vllm_factory.h"
#include "lemon/backends/vllm_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> vllm_create(const BackendContext& ctx) {
    return std::make_unique<VLLMServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
