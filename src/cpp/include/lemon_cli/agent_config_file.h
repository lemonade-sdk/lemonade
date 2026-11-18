#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon_cli {

struct AgentModelEntry {
    std::string id;
    std::string display_name;
    int context_window;
    std::vector<std::string> labels;
};

struct AgentConfigProfile {
    std::string agent_name;
    std::function<std::string()> resolve_config_path;
    std::function<nlohmann::json(const std::string& base_url,
                                 const std::string& api_key,
                                 const std::vector<AgentModelEntry>& models)>
        build_provider_block;
    std::string provider_key = "provider";
    nlohmann::json initial_skeleton = nlohmann::json::object();
};

bool has_label(const AgentModelEntry& model, const std::string& label);

// Atomically write JSON to path, creating parent directories. A symlinked file
// stays linked and an existing file keeps its permissions, since agent config
// can hold other servers' tokens.
bool write_json_atomic(const std::string& path,
                       const nlohmann::json& config,
                       std::string& error_out);

// Reads a JSON object from path into out. A missing file yields an empty
// object; unreadable or malformed files are errors so they are never
// overwritten.
bool read_json_object(const std::string& path, nlohmann::json& out, std::string& error_out);

// Points an MCP server entry at url. Credentials on the entry belong to the
// server it pointed at, so they are dropped when the url changes. The
// Authorization header is key_header when launch exports a key; without one
// only that exact value is removed, keeping a header the user set.
void sync_mcp_server_connection(nlohmann::json& server,
                                const std::string& url,
                                const std::string& key_header,
                                bool has_api_key);

bool sync_agent_config_file(const AgentConfigProfile& profile,
                            const std::string& provider_name,
                            const std::string& base_url,
                            const std::string& api_key,
                            const std::vector<AgentModelEntry>& models,
                            std::string& error_out);

} // namespace lemon_cli
