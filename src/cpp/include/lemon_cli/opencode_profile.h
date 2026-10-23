#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& opencode_profile();

// Where opencode.json keeps the "lemonade" MCP server. opencode 1.0 rejects the
// whole config when mcp has a "servers" entry, and opencode 2 ignores codemode
// on an mcp.<name> entry, so the shape follows the opencode version.
enum class OpencodeMcpShape {
    Legacy,   // mcp.lemonade
    Servers,  // mcp.servers.lemonade
};

// Servers for opencode 2 and newer; Legacy otherwise, including an unknown version.
OpencodeMcpShape opencode_mcp_shape(std::optional<int> major_version);

// Whether opencode.json has a "lemonade" MCP server in either shape.
bool opencode_has_mcp_server();

// Add or refresh the "lemonade" MCP server in opencode.json, pointing at
// <server_origin>/mcp, while keeping other servers and keys. An entry in the
// other shape is removed.
bool sync_opencode_mcp_server(const std::string& server_origin,
                              bool has_api_key,
                              OpencodeMcpShape shape,
                              std::string& error_out);

// Remove the "lemonade" MCP server from opencode.json, in both shapes.
bool remove_opencode_mcp_server(std::string& error_out);

// OPENCODE_CONFIG_CONTENT that turns Code Mode on for the "lemonade" server in
// mcp.servers, or null when there is no such entry.
nlohmann::json opencode_codemode_config();

} // namespace lemon_cli
