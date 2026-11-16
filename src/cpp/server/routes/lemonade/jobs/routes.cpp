#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_jobs_create_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_list_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_pause_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_interrupt_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_resume_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_get_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_jobs_delete_route(ServerContext& ctx);

void register_lemonade_jobs_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_jobs_create_route(ctx));
    registry.add(make_jobs_list_route(ctx));
    registry.add(make_jobs_pause_route(ctx));
    registry.add(make_jobs_interrupt_route(ctx));
    registry.add(make_jobs_resume_route(ctx));
    registry.add(make_jobs_get_route(ctx));
    registry.add(make_jobs_delete_route(ctx));
}

} // namespace lemon
