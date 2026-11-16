#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_chat_completions_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_completions_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_responses_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_embeddings_route(ServerContext& ctx);

void register_openai_text_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_chat_completions_route(ctx));
    registry.add(make_completions_route(ctx));
    registry.add(make_responses_route(ctx));
    registry.add(make_embeddings_route(ctx));
}

} // namespace lemon
