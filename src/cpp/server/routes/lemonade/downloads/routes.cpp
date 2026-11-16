#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_downloads_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_downloads_control_route(ServerContext& ctx);

void register_lemonade_downloads_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_downloads_route(ctx));
    registry.add(make_downloads_control_route(ctx));
}

} // namespace lemon
