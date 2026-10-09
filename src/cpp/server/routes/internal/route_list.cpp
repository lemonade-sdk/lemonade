#include "lemon/server/api_route.h"
#include "lemon/server/route_registry.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class RouteListRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.routes";
        s.methods = {"GET"};
        s.paths = {"routes"};
        s.prefixes = Prefixes::Internal;
        s.summary = "List every route's specification";
        s.description =
            "Returns the specification of every HTTP route lemond serves, in registration "
            "order: its URLs, arguments, and the schema and example request of each response "
            "format.";
        s.notes = {
            "`docs/tools/gen_api_boilerplate.py` reads this endpoint to generate the API "
            "reference, including this page. See the "
            "[HTTP Server Spec](../dev/specs/http-server.md#route-base-classes) for each field.",
        };
        s.args = {
            {"id", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
             "Return only the route with this id, such as `lemonade.health`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id", "methods", "paths", "prefixes", "summary", "description", "notes",
                             "experimental", "args", "responses", "request_format", "model_defaults_to_loaded",
                             "validate_args", "quiet_log"],
                "properties": {
                    "id": {"type": "string", "description": "<page>.<name>; also the docs anchor."},
                    "methods": {"type": "array", "items": {"type": "string"}},
                    "paths": {"type": "array", "items": {"type": "string"}, "description": "Paths without their prefix; entries after the first are aliases."},
                    "prefixes": {"enum": ["Quad", "Internal", "Root"], "description": "Quad serves /api/v0/, /api/v1/, /v0/ and /v1/; Internal serves /internal/; Root serves each path as written."},
                    "summary": {"type": "string"},
                    "description": {"type": "string"},
                    "notes": {"type": "array", "items": {"type": "string"}},
                    "experimental": {"type": "boolean", "description": "The route may still change."},
                    "args": {"type": "array", "items": {
                        "type": "object",
                        "required": ["name", "in", "schema", "required", "supported", "description"],
                        "properties": {
                            "name": {"type": "string"},
                            "in": {"enum": ["JsonBody", "Query", "Path", "Form"]},
                            "schema": {"type": "object"},
                            "required": {"type": "boolean"},
                            "supported": {"enum": ["available", "partial", "not_available"], "description": "not_available documents an argument Lemonade accepts but ignores; partial, one it honors only in part, as its description says."},
                            "description": {"type": "string"}
                        }
                    }},
                    "responses": {"type": "array", "items": {
                        "type": "object",
                        "required": ["format", "schema", "setup", "example"],
                        "properties": {
                            "format": {"enum": ["Json", "JsonLines", "EventStream", "Text", "Binary", "BinaryStream", "Empty"]},
                            "schema": {"description": "JSON Schema of the body, each NDJSON line, or each event's data; null for other formats."},
                            "setup": {"type": "array", "items": {
                                "type": "object",
                                "required": ["route", "format"],
                                "properties": {
                                    "route": {"type": "string"},
                                    "format": {"type": "string"}
                                }
                            }, "description": "Examples that run first to put lemond in the state this one needs."},
                            "example": {"description": "Argument values of a request producing this format."}
                        }
                    }},
                    "request_format": {"enum": ["Raw", "Json", "OptionalJson", "Form"]},
                    "model_defaults_to_loaded": {"type": "boolean"},
                    "validate_args": {"type": "boolean"},
                    "quiet_log": {"type": "boolean"}
                }
            }
        })");
        response.example = {{"id", "internal.telemetry_flush"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        json routes = ctx_.registry->list();
        if (req.http.has_param("id")) {
            const std::string id = req.http.get_param_value("id");
            json matching = json::array();
            for (const auto& route : routes) {
                if (route.value("id", std::string()) == id) {
                    matching.push_back(route);
                }
            }
            routes = std::move(matching);
        }
        res.set_content(routes.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_route_list_route(ServerContext& ctx) {
    return std::make_unique<RouteListRoute>(ctx);
}

} // namespace lemon
