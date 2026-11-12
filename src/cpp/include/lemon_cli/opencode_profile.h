#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& opencode_profile();

// The "lemonade" MCP server lives in mcp.servers.lemonade, which only opencode 2
// reads; opencode 1 rejects the whole config when mcp has a "servers" entry.
bool opencode_has_mcp_server();

// Add or refresh the "lemonade" MCP server in opencode.json, pointing at
// <server_origin>/mcp, while keeping other servers and keys.
bool sync_opencode_mcp_server(const std::string& server_origin,
                              bool has_api_key,
                              std::string& error_out);

bool remove_opencode_mcp_server(std::string& error_out);

// OPENCODE_CONFIG_CONTENT that turns Code Mode on for the "lemonade" server in
// mcp.servers, or null when there is no such entry.
nlohmann::json opencode_codemode_config();

} // namespace lemon_cli
