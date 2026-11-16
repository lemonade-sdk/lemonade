#include "lemon/server/route_registry.h"

namespace lemon {

std::unique_ptr<ApiRoute> make_audio_transcriptions_route(ServerContext& ctx);
std::unique_ptr<ApiRoute> make_audio_speech_route(ServerContext& ctx);

void register_openai_audio_routes(RouteRegistry& registry, ServerContext& ctx) {
    registry.add(make_audio_transcriptions_route(ctx));
    registry.add(make_audio_speech_route(ctx));
}

} // namespace lemon
