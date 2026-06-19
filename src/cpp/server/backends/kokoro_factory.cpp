#include "lemon/backends/kokoro_factory.h"
#include "lemon/backends/kokoro_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> kokoro_create(const BackendContext& ctx) {
    return std::make_unique<KokoroServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
