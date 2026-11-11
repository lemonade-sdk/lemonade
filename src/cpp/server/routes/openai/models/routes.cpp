#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_models_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_id_files_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_id_options_get_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_id_options_post_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_id_options_delete_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_id_route(ServerContext& ctx);

void register_openai_models_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_models_route(ctx));
    // models/{id} matches every longer models/ path, so it registers last.
    registry.add(make_models_id_files_route(ctx));
    registry.add(make_models_id_options_get_route(ctx));
    registry.add(make_models_id_options_post_route(ctx));
    registry.add(make_models_id_options_delete_route(ctx));
    registry.add(make_models_id_route(ctx));
}

} // namespace lemon
