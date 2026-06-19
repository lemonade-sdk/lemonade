#include "lemon/backends/moonshine_factory.h"
#include "lemon/backends/moonshine_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> moonshine_create(const BackendContext& ctx) {
    return std::make_unique<MoonshineServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
