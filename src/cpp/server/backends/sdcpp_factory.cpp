#include "lemon/backends/sdcpp_factory.h"
#include "lemon/backends/sd_server.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> sdcpp_create(const BackendContext& ctx) {
    return std::make_unique<SDServer>(ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

} // namespace backends
} // namespace lemon
