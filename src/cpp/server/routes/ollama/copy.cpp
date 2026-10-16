#include "unsupported.h"

namespace lemon {
namespace {

class CopyRoute : public OllamaUnsupportedRoute {
public:
    using OllamaUnsupportedRoute::OllamaUnsupportedRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.copy";
        s.methods = {"POST"};
        s.paths = {"/api/copy"};
        s.prefixes = Prefixes::Root;
        s.summary = "Copy a model";
        s.errors = {kNotSupported};
        return s;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_copy_route(ServerContext& ctx) {
    return std::make_unique<CopyRoute>(ctx);
}

} // namespace lemon
