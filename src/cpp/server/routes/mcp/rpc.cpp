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
        s.summary = "Model Context Protocol endpoint. JSON-RPC 2.0 envelope. Accepts a single "
                    "message or a batch array.";
        s.description =
            "Serves the Model Context Protocol over JSON-RPC 2.0 (the Streamable HTTP "
            "transport, spec `2025-06-18`), so MCP clients can call Lemonade's models as tools.";
        s.notes = {
            "The body is one JSON-RPC message or a batch array of them. The reply is `200` with "
            "the JSON-RPC response, including JSON-RPC errors; see [Error Model](#error-model). "
            "A request whose messages are all notifications answers `202` with no body.",
            "The MCP specification mandates one endpoint URL per server, so `/mcp` is an exception "
            "to Lemonade's quad-prefix convention.",
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
            "- `resources/*` and `prompts/*` capabilities are not implemented.\n"
            "- Streaming chat output is not exposed via MCP — `stream=true` is ignored. Use `POST "
            "/v1/chat/completions` directly for streamed tokens.\n"
            "- Embeddings and text-to-speech are not currently exposed as MCP tools; use the "
            "OpenAI-compatible endpoints (`/v1/embeddings`, `/v1/audio/speech`) for those.",
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
                "result": {"type": "object", "description": "The method's result; tool failures arrive here with isError: true."},
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
        reply.example = ordered_json::parse(R"({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2025-06-18", "capabilities": {}}
        })");

        RouteResponse notification;
        notification.format = ResponseFormat::Empty;
        notification.example = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};

        s.responses = {reply, notification};
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
