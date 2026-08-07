#pragma once

#include <functional>
#include <memory>
#include <string>
#include <variant>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "model_manager.h"
#include "router.h"

namespace lemon {

using json = nlohmann::json;

// MCP gateway: JSON-RPC 2.0 over HTTP (Streamable HTTP transport, spec 2025-06-18).
//
// /mcp is an INTENTIONAL EXCEPTION to the quad-prefix invariant:
// the MCP spec mandates a single endpoint URL.
class McpServer : public std::enable_shared_from_this<McpServer> {
public:
    using EnsureLoadedFn = std::function<void(const std::string&)>;

    McpServer(Router* router, ModelManager* model_manager, EnsureLoadedFn ensure_loaded);
    ~McpServer();

    // Must be called on a shared_ptr instance — handlers capture shared_from_this().
    void register_routes(httplib::Server& server);

    std::string handle_request_body(const std::string& body);

private:
    json dispatch_message(const json& message);

    json handle_initialize(const json& params, const json& id);
    json handle_tools_list(const json& id);
    json handle_tools_call(const json& params, const json& id);
    json handle_ping(const json& id);

    json tool_chat(const json& arguments);
    json tool_transcribe_audio(const json& arguments);
    json tool_generate_image(const json& arguments);
    json tool_omni(const json& arguments);
    json tool_list_models(const json& arguments);

    // Resolve which model a tool should use when `model` is omitted. Precedence:
    //   1. an explicit `model` argument always wins;
    //   2. a model of the right type that's already LOADED (zero cost);
    //   3. a model of the right type that's already DOWNLOADED (no network);
    //   4. the tool's hard-coded default, but only when the caller passed
    //      `allow_download: true` (the default may be a multi-GB download).
    // Returns the resolved model name, or — when nothing local exists and the
    // caller did not opt into a download — a tool-result JSON (isError=true)
    // asking the agent to supply a `model` or pass `allow_download: true`.
    std::variant<std::string, json> resolve_model_for_tool(
        const json& arguments,
        ModelType want_type,
        const char* type_str,
        const char* default_model,
        bool allow_download);

    static json text_content_block(const std::string& text);
    static json make_error_response(const json& id, int code, const std::string& message);
    static json make_success_response(const json& id, json result);
    // Build the isError=true "no model available, confirm a download" result
    // returned by resolve_model_for_tool (and the Omni handler) when nothing
    // local can satisfy a tool call. `name_hint` describes what kind of model
    // name the agent should supply.
    static json make_needs_model_result(const char* type_str,
                                         const char* default_model,
                                         const std::string& name_hint);
    static json tools_descriptor();

    Router* router_;
    ModelManager* model_manager_;
    EnsureLoadedFn ensure_loaded_;
};

}  // namespace lemon
