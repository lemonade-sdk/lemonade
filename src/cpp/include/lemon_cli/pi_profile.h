#pragma once

#include <string>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& pi_profile();

// The apiKey value written to pi's models.json for a Lemonade API key, which
// may be empty when the server needs none.
std::string pi_api_key_value(const std::string& api_key);

// Check if pi already has a defaultProvider and defaultModel configured.
// Returns true if both are set, false otherwise.
bool pi_has_default_config();

// Pi reads the default provider/model from ~/.pi/agent/settings.json, which is
// separate from the provider definitions in models.json. Write it to set the
// default provider and model for pi.
bool sync_pi_settings_file(const std::string& provider_name,
                           const std::string& default_model,
                           std::string& error_out);

// Whether pi's mcp.json (next to models.json) has a "lemonade" server.
bool pi_has_mcp_server();

// Add or refresh the "lemonade" server in pi's mcp.json, pointing at
// <server_origin>/mcp, while keeping other servers and top-level keys.
bool sync_pi_mcp_server(const std::string& server_origin,
                        bool has_api_key,
                        std::string& error_out);

// Remove the "lemonade" server from pi's mcp.json, leaving everything else.
bool remove_pi_mcp_server(std::string& error_out);

} // namespace lemon_cli
