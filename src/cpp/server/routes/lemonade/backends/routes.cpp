#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_install_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_install_dry_run_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_uninstall_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_cloud_auth_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_cloud_auth_provider_route(ServerContext& ctx);

void register_lemonade_backends_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_install_route(ctx));
    registry.add(make_install_dry_run_route(ctx));
    registry.add(make_uninstall_route(ctx));
    registry.add(make_cloud_auth_route(ctx));
    registry.add(make_cloud_auth_provider_route(ctx));
}

} // namespace lemon
