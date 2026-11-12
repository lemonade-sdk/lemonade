#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_agents_list_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_agents_install_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_agents_uninstall_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_agents_configure_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_agents_launch_route(ServerContext& ctx);

void register_lemonade_agents_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_agents_list_route(ctx));
    registry.add(make_agents_install_route(ctx));
    registry.add(make_agents_uninstall_route(ctx));
    registry.add(make_agents_configure_route(ctx));
    registry.add(make_agents_launch_route(ctx));
}

} // namespace lemon
