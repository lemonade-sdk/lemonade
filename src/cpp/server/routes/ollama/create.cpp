#include "unsupported.h"

namespace lemon {
namespace {

class CreateRoute : public OllamaUnsupportedRoute {
public:
    using OllamaUnsupportedRoute::OllamaUnsupportedRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.create";
        s.methods = {"POST"};
        s.paths = {"/api/create"};
        s.prefixes = Prefixes::Root;
        s.summary = "Create a model from a Modelfile";
        s.description = "Register a model with [`POST /v1/pull`](./lemonade.md#post-v1pull) instead.";
        s.errors = {kNotSupported};
        return s;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_create_route(ServerContext& ctx) {
    return std::make_unique<CreateRoute>(ctx);
}

} // namespace lemon
