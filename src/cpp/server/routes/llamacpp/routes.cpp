#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_rerank_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_slots_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_slots_action_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_tokenize_route(ServerContext& ctx);

void register_llamacpp_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_rerank_route(ctx));
    registry.add(make_slots_route(ctx));
    registry.add(make_slots_action_route(ctx));
    registry.add(make_tokenize_route(ctx));
}

} // namespace lemon
