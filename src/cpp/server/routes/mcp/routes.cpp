#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_mcp_rpc_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_mcp_sse_route(ServerContext& ctx);

void register_mcp_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_mcp_rpc_route(ctx));
    registry.add(make_mcp_sse_route(ctx));
}

} // namespace lemon
