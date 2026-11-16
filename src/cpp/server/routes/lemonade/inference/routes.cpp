#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_classify_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_audio_generations_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_3d_generations_route(ServerContext& ctx);

void register_lemonade_inference_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_classify_route(ctx));
    registry.add(make_audio_generations_route(ctx));
    registry.add(make_3d_generations_route(ctx));
}

} // namespace lemon
