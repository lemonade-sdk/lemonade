#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_live_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_metrics_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_health_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_docs_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_docs_page_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_stats_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_system_stats_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_system_info_route(ServerContext& ctx);

void register_lemonade_system_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_live_route(ctx));
    registry.add(make_metrics_route(ctx));
    registry.add(make_health_route(ctx));
    registry.add(make_docs_route(ctx));
    registry.add(make_docs_page_route(ctx));
    registry.add(make_stats_route(ctx));
    registry.add(make_system_stats_route(ctx));
    registry.add(make_system_info_route(ctx));
}

} // namespace lemon
