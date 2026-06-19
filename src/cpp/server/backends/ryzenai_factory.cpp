#include "lemon/backends/ryzenai_factory.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/model_manager.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

std::unique_ptr<WrappedServer> ryzenai_create(const BackendContext& ctx) {
    // RyzenAI resolves its model path before load (set_model_path), matching the
    // original router factory's special-casing.
    auto server = std::make_unique<::lemon::RyzenAIServer>(
        ctx.model_info->model_name, ctx.log_level == "debug",
        ctx.model_manager, ctx.backend_manager);
    server->set_model_path(ctx.model_info->resolved_path());
    return server;
}

} // namespace backends
} // namespace lemon
