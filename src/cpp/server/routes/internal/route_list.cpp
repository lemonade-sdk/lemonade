#include "lemon/server/api_route.h"
#include "lemon/server/route_registry.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kId{"id", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                   "Return only the route with this id, such as `lemonade.health`."};

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
            "Returns the specification of every HTTP route `lemond` serves, in registration "
            "order: its URLs, arguments, the schema of each response format, and its example "
            "requests.";
        s.notes = {
            "`docs/tools/gen_api_boilerplate.py` reads this endpoint to generate the API "
            "reference, including this page. See the "
            "[HTTP Server Spec](../dev/specs/http-server.md#route-base-classes) for each field.",
        };
        s.args = {kId};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id", "methods", "paths", "prefixes", "summary", "modality", "description",
                             "notes", "experimental", "args", "responses", "examples", "errors", "error_format",
                             "request_format", "quiet_log"],
                "properties": {
                    "id": {"type": "string", "description": "`<page>.<name>`; names the route's generated region in `docs/api/<page>.md`."},
                    "methods": {"type": "array", "items": {"type": "string"}, "description": "HTTP methods the route answers, such as `POST`."},
                    "paths": {"type": "array", "items": {"type": "string"}, "description": "Paths without their prefix; entries after the first are aliases."},
                    "prefixes": {"enum": ["Quad", "Internal", "Root"], "description": "`Quad` serves each path under `/v1/` and older prefixes kept for existing clients; `Internal` under `/internal/`; `Root` as written."},
                    "summary": {"type": "string", "description": "One-line summary for the page's summary table; empty when hand-written rows replace it."},
                    "modality": {"type": "string", "description": "The route's input and output for the summary table's Modality column, such as `messages -> completion`; empty on pages without that column."},
                    "description": {"type": "string", "description": "Reference text for the route's section."},
                    "notes": {"type": "array", "items": {"type": "string"}, "description": "Sections the docs place after the Parameters table."},
                    "experimental": {"type": "boolean", "description": "The route may still change."},
                    "args": {"type": "array", "description": "Every argument the route takes.", "items": {
                        "type": "object",
                        "required": ["name", "in", "schema", "required", "supported", "description", "aliases",
                                     "forwarded"],
                        "properties": {
                            "name": {"type": "string", "description": "The argument's name."},
                            "in": {"enum": ["JsonBody", "Query", "Path", "Form"], "description": "Where the argument goes: the JSON body, the query string, the path, or a form field."},
                            "schema": {"type": "object", "description": "JSON Schema of the argument's value."},
                            "required": {"type": "boolean", "description": "Whether a request must send the argument."},
                            "supported": {"enum": ["available", "partial", "not_available"], "description": "`available`; `partial`, honored only in part as the argument's description says; or `not_available`, accepted but ignored."},
                            "description": {"type": "string", "description": "What the argument controls."},
                            "aliases": {"type": "array", "items": {"type": "string"}, "description": "Other names a request may send the argument under."},
                            "forwarded": {"type": "boolean", "description": "Whether `lemond` passes the argument to the backend unchecked."}
                        }
                    }},
                    "responses": {"type": "array", "description": "One entry per response format the route returns.", "items": {
                        "type": "object",
                        "required": ["format", "schema"],
                        "properties": {
                            "format": {"enum": ["Json", "JsonLines", "EventStream", "Text", "Binary", "BinaryStream", "Empty"], "description": "The response format."},
                            "schema": {"description": "JSON Schema of the body, each NDJSON line, or each event's data; null for other formats."}
                        }
                    }},
                    "examples": {"type": "array", "items": {
                        "type": "object",
                        "required": ["name", "description", "format", "args", "setup"],
                        "properties": {
                            "name": {"type": "string", "description": "Subsection heading; empty when the route has one example."},
                            "description": {"type": "string", "description": "What the example shows, opening its subsection; empty when the route's other text covers it."},
                            "format": {"type": "string", "description": "The response format the reply is validated against."},
                            "args": {"description": "Argument values of the request."},
                            "setup": {"type": "array", "items": {
                                "type": "object",
                                "required": ["route", "example", "args"],
                                "properties": {
                                    "route": {"type": "string", "description": "The `id` of the route the step requests."},
                                    "example": {"type": "string", "description": "The name of the route's example to run."},
                                    "args": {"description": "Set instead of example: a request sent as is and left out of the docs."}
                                }
                            }, "description": "Requests that run first to put `lemond` in the state this example needs."}
                        }
                    }, "description": "The first is the route's main example."},
                    "errors": {"type": "array", "description": "Every error status the route answers with, including those its base class adds.", "items": {
                        "type": "object",
                        "required": ["status", "code", "when"],
                        "properties": {
                            "status": {"type": "integer", "description": "The HTTP status."},
                            "code": {"type": "string", "description": "The code the error body carries, or empty when it carries none."},
                            "when": {"type": "string", "description": "The condition that produces the error."}
                        }
                    }},
                    "error_format": {"enum": ["Plain", "OpenAi", "Anthropic"], "description": "The body shape of the errors `lemond` writes when a request is not valid JSON or an argument is missing or invalid."},
                    "request_format": {"enum": ["Raw", "Json", "OptionalJson", "Form"], "description": "How `lemond` parses the request body: `Raw` not at all, `Json` as JSON, `OptionalJson` as JSON with an empty or `null` body read as `{}`, `Form` as a multipart form."},
                    "quiet_log": {"type": "boolean", "description": "Whether the route is left out of the access log."}
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "internal.telemetry_flush"}};
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        auto routes = ctx_.registry->list();
        if (req.has(kId)) {
            const std::string id = req.arg(kId).get<std::string>();
            auto matching = ordered_json::array();
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
