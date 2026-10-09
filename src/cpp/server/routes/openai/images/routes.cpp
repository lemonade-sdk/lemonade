#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_images_generations_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_images_edits_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_images_variations_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_images_upscale_route(ServerContext& ctx);

void register_openai_images_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_images_generations_route(ctx));
    registry.add(make_images_edits_route(ctx));
    registry.add(make_images_variations_route(ctx));
    registry.add(make_images_upscale_route(ctx));
}

} // namespace lemon
