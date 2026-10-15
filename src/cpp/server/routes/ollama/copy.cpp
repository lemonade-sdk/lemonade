#include "lemon/server/api_route.h"

namespace lemon {
namespace {

const RouteError kNotSupported{501, "", "Lemonade does not support this Ollama operation"};

class CopyRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

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

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = kNotSupported.status;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_copy_route(ServerContext& ctx) {
    return std::make_unique<CopyRoute>(ctx);
}

} // namespace lemon
