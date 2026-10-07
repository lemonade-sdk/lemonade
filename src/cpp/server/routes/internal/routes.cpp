#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_shutdown_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_telemetry_flush_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_pin_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_set_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_config_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_config_defaults_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_cleanup_cache_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_simulate_vram_pressure_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_sync_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_models_sync_status_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_aliases_list_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_aliases_create_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_aliases_delete_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_route_list_route(ServerContext& ctx);

void register_internal_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_shutdown_route(ctx));
    registry.add(make_telemetry_flush_route(ctx));
    registry.add(make_pin_route(ctx));
    registry.add(make_set_route(ctx));
    registry.add(make_config_route(ctx));
    registry.add(make_config_defaults_route(ctx));
    registry.add(make_cleanup_cache_route(ctx));
    registry.add(make_simulate_vram_pressure_route(ctx));
    registry.add(make_models_sync_route(ctx));
    registry.add(make_models_sync_status_route(ctx));
    registry.add(make_aliases_list_route(ctx));
    registry.add(make_aliases_create_route(ctx));
    registry.add(make_aliases_delete_route(ctx));
    registry.add(make_route_list_route(ctx));
}

} // namespace lemon
