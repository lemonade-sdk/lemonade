#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_anthropic_messages_route(ServerContext& ctx);

void register_anthropic_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_anthropic_messages_route(ctx));
}

} // namespace lemon
