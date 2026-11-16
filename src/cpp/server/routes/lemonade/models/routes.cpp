#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_models_check_updates_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_register_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_pull_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_pull_variants_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_registry_search_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_delete_route(ServerContext& ctx);

void register_lemonade_models_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_models_check_updates_route(ctx));
    registry.add(make_models_register_route(ctx));
    registry.add(make_pull_route(ctx));
    registry.add(make_pull_variants_route(ctx));
    registry.add(make_registry_search_route(ctx));
    registry.add(make_delete_route(ctx));
}

} // namespace lemon
