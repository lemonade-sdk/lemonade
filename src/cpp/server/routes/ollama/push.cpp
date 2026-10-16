#include "unsupported.h"

namespace lemon {
namespace {

class PushRoute : public OllamaUnsupportedRoute {
public:
    using OllamaUnsupportedRoute::OllamaUnsupportedRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.push";
        s.methods = {"POST"};
        s.paths = {"/api/push"};
        s.prefixes = Prefixes::Root;
        s.summary = "Push a model to a registry";
        s.errors = {kNotSupported};
        return s;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_push_route(ServerContext& ctx) {
    return std::make_unique<PushRoute>(ctx);
}

} // namespace lemon
