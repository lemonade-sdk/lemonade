#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_videos_create_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_videos_get_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_videos_content_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_videos_cancel_route(ServerContext& ctx);

void register_lemonade_videos_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_videos_create_route(ctx));
    registry.add(make_videos_get_route(ctx));
    registry.add(make_videos_content_route(ctx));
    registry.add(make_videos_cancel_route(ctx));
}

} // namespace lemon
