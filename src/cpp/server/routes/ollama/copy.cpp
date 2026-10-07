#include "lemon/server/api_route.h"

namespace lemon {
namespace {

class CopyRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.copy";
        s.methods = {"POST"};
        s.paths = {"/api/copy"};
        s.prefixes = Prefixes::Root;
        s.summary = "Copy a model (not supported)";
        s.description = "Not supported: answers `501`. A model alias gives a model a second name instead; see [`POST /internal/aliases`](./internal.md#post-internalaliases).";
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = 501;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_copy_route(ServerContext& ctx) {
    return std::make_unique<CopyRoute>(ctx);
}

} // namespace lemon
