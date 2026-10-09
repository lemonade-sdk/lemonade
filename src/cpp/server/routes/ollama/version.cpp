#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class VersionRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.version";
        s.methods = {"GET"};
        s.paths = {"/api/version"};
        s.prefixes = Prefixes::Root;
        s.summary = "Version";
        s.description =
            "Reports an Ollama version recent enough for Ollama clients' version checks. "
            "[`GET /v1/health`](./lemonade.md#get-v1health) reports Lemonade's own version.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["version"],
            "properties": {"version": {"type": "string"}}
        })");
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        json response = {{"version", "0.16.1"}};
        res.set_content(response.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_version_route(ServerContext& ctx) {
    return std::make_unique<VersionRoute>(ctx);
}

} // namespace lemon
