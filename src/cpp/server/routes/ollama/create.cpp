#include "lemon/server/api_route.h"

namespace lemon {
namespace {

const RouteError kNotSupported{501, "", "Lemonade does not support this Ollama operation"};

class CreateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

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

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = kNotSupported.status;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_create_route(ServerContext& ctx) {
    return std::make_unique<CreateRoute>(ctx);
}

} // namespace lemon
