#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_ollama_chat_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_generate_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_tags_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_show_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_delete_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_pull_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_embed_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_embeddings_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_ps_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_version_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_create_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_copy_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_push_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_ollama_blobs_route(ServerContext& ctx);

void register_ollama_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_ollama_chat_route(ctx));
    registry.add(make_ollama_generate_route(ctx));
    registry.add(make_ollama_tags_route(ctx));
    registry.add(make_ollama_show_route(ctx));
    registry.add(make_ollama_delete_route(ctx));
    registry.add(make_ollama_pull_route(ctx));
    registry.add(make_ollama_embed_route(ctx));
    registry.add(make_ollama_embeddings_route(ctx));
    registry.add(make_ollama_ps_route(ctx));
    registry.add(make_ollama_version_route(ctx));
    registry.add(make_ollama_create_route(ctx));
    registry.add(make_ollama_copy_route(ctx));
    registry.add(make_ollama_push_route(ctx));
    registry.add(make_ollama_blobs_route(ctx));
}

} // namespace lemon
