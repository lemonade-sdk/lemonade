#include "lemon/backends/cloud_factory.h"
#include "lemon/backends/cloud_server.h"
#include "lemon/model_manager.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> cloud_create(const BackendContext& ctx) {
    return std::make_unique<CloudServer>(
        ctx.model_info->cloud_provider, ctx.log_level,
        ctx.model_manager, ctx.backend_manager, ctx.cloud_registry);
}

} // namespace backends
} // namespace lemon
