#include <lemon/utils/aixlog.hpp>

#include "lemon/mcp_server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class McpRpcRoute : public ApiRoute {
public:
    explicit McpRpcRoute(ServerContext& ctx)
        : ApiRoute(ctx),
          mcp_(ctx.router, ctx.model_manager,
               [loader = ctx.model_loader](const std::string& model) {
                   loader->ensure_loaded(model);
               }) {}

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "mcp.rpc";
        s.methods = {"POST"};
        s.paths = {"/mcp"};
        s.prefixes = Prefixes::Root;
        s.summary = "Model Context Protocol over JSON-RPC 2.0";
        s.description = "Serves every MCP method over JSON-RPC 2.0.";
        s.notes = {
            "The body is one JSON-RPC message or a batch array of them. The reply is `200` with "
            "the JSON-RPC response, including JSON-RPC errors; see [Error Model](#error-model). "
            "A request whose messages are all notifications answers `202` with no body.",
            "The MCP specification mandates one endpoint URL per server, so `/mcp` has no "
            "`/api/v1/`, `/v1/` or other prefixed form.",
            "### Supported methods\n"
            "\n"
            "| Method | Purpose |\n"
            "|--------|---------|\n"
            "| `initialize` | Negotiate protocol version, return server identity and capabilities. |\n"
            "| `notifications/initialized` | Client acknowledgement; silently accepted. |\n"
            "| `tools/list` | Return the catalogue of callable tools (with JSON Schemas). |\n"
            "| `tools/call` | Invoke one of the tools below. |\n"
            "| `ping` | Liveness probe; returns `{}`. |",
            "### Error model\n"
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
            "- Embeddings and text-to-speech have no MCP tool; use "
            "[`/v1/embeddings`](./openai.md#post-v1embeddings) and "
            "[`/v1/audio/speech`](./openai.md#post-v1audiospeech).",
        };
        s.args = {
            {"jsonrpc", ArgIn::JsonBody, {{"const", "2.0"}}, true, Support::Available,
             "JSON-RPC version: `\"2.0\"`."},
            {"id", ArgIn::JsonBody, {{"type", json::array({"string", "integer"})}}, false, Support::Available,
             "Request id, echoed in the reply. Omit it to send a notification, which gets no "
             "reply."},
            {"method", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "One of the [supported methods](#supported-methods), e.g. `initialize`, "
             "`tools/list` or `tools/call`."},
            {"params", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
             "Method parameters; `tools/call` takes `name` and `arguments`."},
        };

        RouteResponse reply;
        reply.format = ResponseFormat::Json;
        reply.schema = ordered_json::parse(R"({
            "type": ["object", "array"],
            "description": "One JSON-RPC 2.0 response, or an array of them for a batch.",
            "required": ["jsonrpc", "id"],
            "properties": {
                "jsonrpc": {"const": "2.0"},
                "id": {"type": ["string", "integer", "null"]},
                "result": {"type": "object", "description": "The method's result."},
                "error": {
                    "type": "object",
                    "description": "A JSON-RPC error, such as -32601 for an unknown method.",
                    "required": ["code", "message"],
                    "properties": {
                        "code": {"type": "integer"},
                        "message": {"type": "string"}
                    }
                }
            }
        })");

        RouteResponse notification;
        notification.format = ResponseFormat::Empty;

        s.responses = {reply, notification};

        RouteExample reply_example;
        reply_example.name = "JSON";
        reply_example.format = ResponseFormat::Json;
        reply_example.args = ordered_json::parse(R"({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {}}
        })");

        RouteExample notification_example;
        notification_example.name = "empty";
        notification_example.format = ResponseFormat::Empty;
        notification_example.args = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};

        s.examples = {reply_example, notification_example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string response_body;
        try {
            response_body = mcp_.handle_request_body(req.http.body);
        } catch (const std::exception& e) {
            LOG(ERROR, "McpServer") << "Unhandled exception in POST /mcp: " << e.what() << std::endl;
            res.status = 500;
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
