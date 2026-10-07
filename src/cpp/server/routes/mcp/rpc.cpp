#include <lemon/utils/aixlog.hpp>

#include "lemon/mcp_server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
        s.summary = "Model Context Protocol endpoint";
        s.description =
            "Serves the Model Context Protocol over JSON-RPC 2.0 (the Streamable HTTP "
            "transport, spec `2025-06-18`), so MCP clients can call Lemonade's models as tools.";
        s.notes = {
            "The MCP specification mandates a single endpoint URL, so `/mcp` has no `/v1` "
            "prefix.",
            "The body is one JSON-RPC message or a batch array of them. The reply is `200` with "
            "the JSON-RPC response, including JSON-RPC errors; see [Error Model](#error-model). "
            "A request whose messages are all notifications answers `202` with no body.",
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
        reply.schema = json::parse(R"({
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
        reply.example = json::parse(R"({
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
