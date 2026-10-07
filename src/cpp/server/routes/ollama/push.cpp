#include "lemon/server/api_route.h"

namespace lemon {
namespace {

class PushRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.push";
        s.methods = {"POST"};
        s.paths = {"/api/push"};
        s.prefixes = Prefixes::Root;
        s.summary = "Push a model to a registry (not supported)";
        s.description = "Not supported: answers `501`.";
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = 501;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_push_route(ServerContext& ctx) {
    return std::make_unique<PushRoute>(ctx);
}

} // namespace lemon
