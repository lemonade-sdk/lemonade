#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_routing_validate_route(ServerContext& ctx);

void register_router_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_routing_validate_route(ctx));
}

} // namespace lemon
