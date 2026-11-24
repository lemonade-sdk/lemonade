#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/model_json.h"
#include "lemon/server/server_context.h"
#include "lemon/server.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/mcp_server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInternalError{
    500, "-32603",
    "An exception escapes the JSON-RPC handling, such as a number too large for a double or "
    "invalid UTF-8 in the body"};

const RouteArg kJsonrpc{"jsonrpc", ArgIn::JsonBody, {{"const", "2.0"}}, true, Support::Available,
                        "JSON-RPC version: `\"2.0\"`."};
const RouteArg kId{"id", ArgIn::JsonBody, {{"type", json::array({"string", "integer"})}}, false, Support::Available,
                   "Request id, echoed in the reply. Omit it to send a notification, which gets no "
                   "reply."};
const RouteArg kMethod{"method", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                       "One of the [supported methods](#supported-methods), e.g. `initialize`, "
                       "`tools/list` or `tools/call`."};
const RouteArg kParams{"params", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
                       "Method parameters; `tools/call` takes `name` and `arguments`."};

std::string mcp_model_result_text(const nlohmann::json& payload) {
    if (payload.is_object()) {
        auto error = payload.find("error");
        if (error != payload.end()) {
            if (error->is_string()) return error->get<std::string>();
            if (error->is_object()) {
                auto message = error->find("message");
                if (message != error->end() && message->is_string()) {
                    return message->get<std::string>();
                }
            }
        }
        auto message = payload.find("message");
        if (message != payload.end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    if (payload.is_string()) return payload.get<std::string>();
    return payload.dump();
}

nlohmann::json mcp_model_tool_result(nlohmann::json payload,
                                     bool is_error,
                                     const std::string& message = "") {
    nlohmann::json structured = payload.is_object()
        ? payload
        : nlohmann::json{{"result", std::move(payload)}};
    const std::string text = message.empty()
        ? mcp_model_result_text(structured)
        : message;
    return {
        {"content", nlohmann::json::array({{
            {"type", "text"},
            {"text", text},
        }})},
        {"structuredContent", std::move(structured)},
        {"isError", is_error},
    };
}

nlohmann::json mcp_model_http_result(const httplib::Response& response) {
    nlohmann::json payload;
    if (response.body.empty()) {
        payload = nlohmann::json::object();
    } else {
        try {
            payload = nlohmann::json::parse(response.body);
        } catch (const std::exception&) {
            payload = {{"body", response.body}};
        }
    }
    if (!payload.is_object()) {
        payload = {{"result", std::move(payload)}};
    }
    if (response.status > 0) {
        payload["http_status"] = response.status;
    }
    return mcp_model_tool_result(std::move(payload), response.status >= 400);
}

void validate_mcp_model_arguments(
        const nlohmann::json& arguments,
        const std::set<std::string>& allowed) {
    if (!arguments.is_object()) {
        throw std::invalid_argument("Tool arguments must be an object");
    }
    for (const auto& [key, value] : arguments.items()) {
        (void)value;
        if (allowed.count(key) == 0) {
            throw std::invalid_argument("Unknown argument: " + key);
        }
    }
}

std::string required_mcp_model_string(const nlohmann::json& arguments,
                                      const char* key) {
    auto value = arguments.find(key);
    if (value == arguments.end() || !value->is_string()) {
        throw std::invalid_argument(std::string("Missing or non-string argument: ") + key);
    }
    std::string result = value->get<std::string>();
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    result.erase(std::find_if(result.rbegin(), result.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), result.end());
    if (result.empty()) {
        throw std::invalid_argument(std::string("Argument must not be empty: ") + key);
    }
    return result;
}

std::int64_t parse_mcp_ctx_size(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("ctx_size exceeds int64 range");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (!std::isfinite(number) || std::trunc(number) != number ||
            number < static_cast<double>(
                std::numeric_limits<std::int64_t>::min()) ||
            number >= std::ldexp(1.0, 63)) {
            throw std::invalid_argument(
                "ctx_size must be an int64-representable integer");
        }
        return static_cast<std::int64_t>(number);
    }
    throw std::invalid_argument("ctx_size must be an integer");
}

nlohmann::json call_mcp_server_capability(ServerContext& ctx,
                                          const std::string& tool_name,
                                          const nlohmann::json& arguments) {
    auto call_rest = [&ctx](const std::string& method,
                            const std::string& path,
                            const nlohmann::json& body,
                            httplib::Response& response) {
        httplib::Request request;
        request.method = method;
        request.path = path;
        if (method == "POST") {
            request.body = body.dump();
            request.headers.emplace("Content-Type", "application/json");
        }
        // The same ApiRoute and service implementation that serves HTTP.
        // /mcp already crossed Lemonade's regular API-key boundary.
        ctx.server->handle_request(request, response);
    };

    auto call_json_handler = [&call_rest](const nlohmann::json& body,
                                         const char* path) {
        httplib::Response response;
        call_rest("POST", path, body, response);
        return mcp_model_http_result(response);
    };

    auto call_get_handler = [&call_rest](const char* path) {
        httplib::Response response;
        call_rest("GET", path, nlohmann::json::object(), response);
        return mcp_model_http_result(response);
    };

    try {
        if (tool_name == "lemonade_get_model_info") {
            validate_mcp_model_arguments(arguments, {"model_name"});
            const std::string requested = required_mcp_model_string(arguments, "model_name");
            if (!ctx.model_manager->model_exists(requested)) {
                return mcp_model_tool_result({
                    {"error", "Model not found: " + requested},
                    {"model_name", requested},
                }, true);
            }
            const std::string model_name = ctx.model_manager->resolve_model_name(requested);
            const ModelInfo info = ctx.model_manager->get_model_info(model_name);
            const std::string public_name = ctx.model_manager->get_public_model_name(model_name);
            const std::string response_name = public_name.empty() ? requested : public_name;
            nlohmann::json payload = ctx.model_json->to_json(response_name, info);
            payload["model_name"] = response_name;
            return mcp_model_tool_result(std::move(payload), false);
        }

        if (tool_name == "lemonade_load_model") {
            validate_mcp_model_arguments(
                arguments, {"model_name", "ctx_size", "pinned", "options"});
            nlohmann::json body = {
                {"model_name", required_mcp_model_string(arguments, "model_name")},
                {"save_options", false},
            };

            if (arguments.contains("ctx_size")) {
                body["ctx_size"] = parse_mcp_ctx_size(arguments["ctx_size"]);
            }
            if (arguments.contains("pinned")) {
                if (!arguments["pinned"].is_boolean()) {
                    throw std::invalid_argument("pinned must be a boolean");
                }
                body["pinned"] = arguments["pinned"];
            }
            if (arguments.contains("options")) {
                if (!arguments["options"].is_object()) {
                    throw std::invalid_argument("options must be an object");
                }
                for (const auto& [key, value] : arguments["options"].items()) {
                    if (key == "model" || key == "model_name" ||
                        key == "save_options" || key == "pinned") {
                        throw std::invalid_argument(
                            "options must contain recipe options only; reserved key: " + key);
                    }
                    if (key == "ctx_size" && arguments.contains("ctx_size")) {
                        throw std::invalid_argument(
                            "ctx_size must be provided either at top level or in options, not both");
                    }
                    if (key == "ctx_size") {
                        body[key] = parse_mcp_ctx_size(value);
                        continue;
                    }
                    body[key] = value;
                }
            }
            return call_json_handler(body, "/api/v1/load");
        }

        if (tool_name == "lemonade_unload_model") {
            validate_mcp_model_arguments(arguments, {"model_name"});
            const nlohmann::json body = {
                {"model_name", required_mcp_model_string(arguments, "model_name")},
            };
            return call_json_handler(body, "/api/v1/unload");
        }

        if (tool_name == "lemonade_pull_model") {
            validate_mcp_model_arguments(
                arguments,
                {"model_name", "checkpoint", "recipe", "source", "do_not_upgrade"});
            nlohmann::json body = {
                {"model_name", required_mcp_model_string(arguments, "model_name")},
                {"stream", false},
            };
            for (const char* key : std::vector<const char*>{"checkpoint", "recipe", "source"}) {
                if (!arguments.contains(key)) continue;
                if (!arguments[key].is_string()) {
                    throw std::invalid_argument(std::string(key) + " must be a string");
                }
                body[key] = arguments[key];
            }
            if (arguments.contains("do_not_upgrade")) {
                if (!arguments["do_not_upgrade"].is_boolean()) {
                    throw std::invalid_argument("do_not_upgrade must be a boolean");
                }
                body["do_not_upgrade"] = arguments["do_not_upgrade"];
            }
            return call_json_handler(body, "/api/v1/pull");
        }

        if (tool_name == "lemonade_delete_model") {
            validate_mcp_model_arguments(arguments, {"model_name", "confirm"});
            const std::string requested = required_mcp_model_string(arguments, "model_name");
            if (!arguments.contains("confirm") || !arguments["confirm"].is_boolean() ||
                !arguments["confirm"].get<bool>()) {
                return mcp_model_tool_result({
                    {"error", "Deletion requires confirm=true"},
                    {"model_name", requested},
                    {"required", "confirm=true"},
                }, true);
            }

            if (!ctx.model_manager->model_exists(requested)) {
                return mcp_model_tool_result({
                    {"error", "Model not found: " + requested},
                    {"model_name", requested},
                }, true);
            }
            return call_json_handler(
                nlohmann::json{{"model_name", requested}}, "/api/v1/delete");
        }

        if (tool_name == "lemonade_get_loaded_models") {
            validate_mcp_model_arguments(arguments, {});
            nlohmann::json models = ctx.router->get_all_loaded_models();
            return mcp_model_tool_result({
                {"models", models},
                {"count", models.size()},
            }, false);
        }

        if (tool_name == "lemonade_get_server_health") {
            validate_mcp_model_arguments(arguments, {});
            return call_get_handler("/api/v1/health");
        }

        if (tool_name == "lemonade_get_system_info") {
            validate_mcp_model_arguments(arguments, {});
            return call_get_handler("/api/v1/system-info");
        }

        if (tool_name == "lemonade_list_backends") {
            validate_mcp_model_arguments(arguments, {});
            httplib::Request request;
            request.method = "GET";
            httplib::Response response;
            call_rest("GET", "/api/v1/system-info", nlohmann::json::object(), response);
            if (response.status >= 400) return mcp_model_http_result(response);
            nlohmann::json system_info = nlohmann::json::parse(response.body);
            if (!system_info.contains("recipes")) {
                return mcp_model_tool_result({
                    {"error", "Backend recipe detection unavailable: recipes missing from system-info"},
                }, true);
            }
            nlohmann::json payload = {
                {"recipes", system_info.at("recipes")},
                {"unavailable_recipes", system_info.value(
                    "unavailable_recipes", nlohmann::json::array())},
                {"no_fetch_executables", system_info.value("no_fetch_executables", false)},
            };
            return mcp_model_tool_result(std::move(payload), false);
        }

        if (tool_name == "lemonade_install_backend") {
            validate_mcp_model_arguments(arguments, {"recipe", "backend", "force"});
            nlohmann::json body = {
                {"recipe", required_mcp_model_string(arguments, "recipe")},
                {"backend", required_mcp_model_string(arguments, "backend")},
                {"stream", false},
            };
            if (arguments.contains("force")) {
                if (!arguments["force"].is_boolean()) {
                    throw std::invalid_argument("force must be a boolean");
                }
                body["force"] = arguments["force"];
            }
            return call_json_handler(body, "/api/v1/install");
        }

        return mcp_model_tool_result({{"error", "Unknown Lemonade server tool: " + tool_name}}, true);
    } catch (const std::exception& e) {
        return mcp_model_tool_result({{"error", e.what()}}, true);
    }
}

class McpRpcRoute : public ApiRoute {
public:
    explicit McpRpcRoute(ServerContext& ctx)
        : ApiRoute(ctx),
          mcp_(ctx.router, ctx.model_manager,
               [loader = ctx.model_loader](const std::string& model) {
                   loader->ensure_loaded(model);
               },
               [&ctx](const std::string& name, const json& args) {
                   return call_mcp_server_capability(ctx, name, args);
               }) {}

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "mcp.rpc";
        s.methods = {"POST"};
        s.paths = {"/mcp"};
        s.prefixes = Prefixes::Root;
        s.summary = "Call MCP methods over JSON-RPC 2.0";
        s.description =
            "Serves the MCP methods listed under [Supported Methods](#supported-methods) over "
            "JSON-RPC 2.0.";
        s.notes = {
            "The body is one JSON-RPC message or a batch array of them. A JSON-RPC error is "
            "answered with `200` like any other JSON-RPC response; see [Error Model](#error-model). "
            "A request whose messages are all notifications answers `202` with no body.",
            "### Supported Methods\n"
            "\n"
            "| Method | Purpose |\n"
            "|--------|---------|\n"
            "| `initialize` | Return the server's protocol version, identity and capabilities. |\n"
            "| `notifications/initialized` | Client acknowledgement; silently accepted. |\n"
            "| `notifications/cancelled` | Client cancellation; silently accepted. |\n"
            "| `tools/list` | Return the catalogue of callable tools (with JSON Schemas). |\n"
            "| `tools/call` | Invoke one of the tools below. |\n"
            "| `ping` | Liveness probe; returns `{}`. |",
            "### Error Model\n"
            "\n"
            "| Code | Meaning |\n"
            "|------|---------|\n"
            "| `-32700` | Body was not valid JSON. |\n"
            "| `-32600` | Request was not a JSON-RPC object (or batch was empty / missing `method`). |\n"
            "| `-32601` | Unknown JSON-RPC method (e.g. `resources/list`). |\n"
            "| `-32602` | Invalid `params` for a known method. |\n"
            "| `-32603` | Internal server error (an exception escaped a handler). |\n"
            "\n"
            "Tool-level failures (bad arguments, model load errors, backend exceptions) are "
            "returned as **successful** JSON-RPC results with `\"isError\": true` and a text content "
            "block describing the failure, so MCP-aware models can self-correct.",
            "### Limitations\n"
            "\n"
            "- No session resumption (`Mcp-Session-Id` header is not issued).\n"
            "- Tools return their whole result: `lemonade_chat` ignores `stream`. Use "
            "[`POST /v1/chat/completions`](./openai.md#post-v1chatcompletions) for streamed tokens.\n"
            "- Embeddings have no MCP tool; use "
            "[`/v1/embeddings`](./openai.md#post-v1embeddings).",
        };
        s.args = {kJsonrpc, kId, kMethod, kParams};

        RouteResponse reply;
        reply.format = ResponseFormat::Json;
        reply.schema = ordered_json::parse(R"({
            "type": ["object", "array"],
            "description": "One JSON-RPC 2.0 response, or an array of them for a batch.",
            "required": ["jsonrpc", "id"],
            "properties": {
                "jsonrpc": {"const": "2.0", "description": "Always `2.0`."},
                "id": {"type": ["string", "integer", "null"], "description": "The `id` of the request this answers; `null` when it could not be read."},
                "result": {"type": "object", "description": "The method's result."},
                "error": {
                    "type": "object",
                    "description": "A JSON-RPC error, such as `-32601` for an unknown method.",
                    "required": ["code", "message"],
                    "properties": {
                        "code": {"type": "integer", "description": "The JSON-RPC error code."},
                        "message": {"type": "string", "description": "What went wrong."}
                    }
                }
            }
        })");

        RouteResponse notification;
        notification.format = ResponseFormat::Empty;

        s.responses = {reply, notification};

        RouteExample reply_example;
        reply_example.name = "request";
        reply_example.format = ResponseFormat::Json;
        reply_example.args = ordered_json::parse(R"({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {}}
        })");

        RouteExample notification_example;
        notification_example.name = "notification";
        notification_example.format = ResponseFormat::Empty;
        notification_example.args = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};

        s.examples = {reply_example, notification_example};
        s.errors = {kInternalError};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string response_body;
        try {
            response_body = mcp_.handle_request_body(req.http.body);
        } catch (const std::exception& e) {
            LOG(ERROR, "McpServer") << "Unhandled exception in POST /mcp: " << e.what() << std::endl;
            res.status = kInternalError.status;
            res.set_content(McpServer::internal_error_response(e.what()).dump(), "application/json");
            return;
        }

        if (response_body.empty()) {
            res.status = 202;
            return;
        }

        res.status = 200;
        res.set_content(response_body, "application/json");
    }

private:
    McpServer mcp_;
};

} // namespace

std::unique_ptr<ApiRoute> make_mcp_rpc_route(ServerContext& ctx) {
    return std::make_unique<McpRpcRoute>(ctx);
}

} // namespace lemon
