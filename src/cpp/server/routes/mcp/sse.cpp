#include "lemon/server/api_route.h"

namespace lemon {
namespace {

const RouteError kGetNotSupported{
    405, "-32600", "Always, since MCP calls go to `POST /mcp`; the reply carries `Allow: POST`"};

class McpSseRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "mcp.sse";
        s.methods = {"GET"};
        s.paths = {"/mcp"};
        s.prefixes = Prefixes::Root;
        s.summary = "Open a server-initiated event stream";
        s.description =
            "Lemonade opens no server-initiated event stream, so tools return their full result "
            "in the [`POST /mcp`](#post-mcp) reply.";
        s.errors = {kGetNotSupported};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = kGetNotSupported.status;
        res.set_header("Allow", "POST");
        res.set_content(
            "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,"
            "\"message\":\"GET /mcp not supported; use POST with a JSON-RPC body\"},"
            "\"id\":null}",
            "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_mcp_sse_route(ServerContext& ctx) {
    return std::make_unique<McpSseRoute>(ctx);
}

} // namespace lemon
