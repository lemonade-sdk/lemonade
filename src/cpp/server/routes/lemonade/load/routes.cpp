#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_load_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_unload_route(ServerContext& ctx);

void register_lemonade_load_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_load_route(ctx));
    registry.add(make_unload_route(ctx));
}

} // namespace lemon
