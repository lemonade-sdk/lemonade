#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Server-side MCP client host for external MCP servers.
//
// This foundation intentionally provides only stdio transport, persistent server
// configuration, explicit connect/disconnect, tool discovery, and raw tool calls.
// Chat-loop orchestration and GUI selection remain separate follow-up work.
struct McpServerConfig {
    std::string id;
    std::string name;
    std::string transport = "stdio";
    std::string command;
    std::vector<std::string> args;
    // Values must be environment references in the form ${VARIABLE}. Raw values
    // are rejected so credentials can never be persisted in mcp_servers.json.
    std::map<std::string, std::string> env;
    std::string working_dir;
    bool enabled = true;
    int timeout_ms = 30000;
};

class McpClientManager : public std::enable_shared_from_this<McpClientManager> {
public:
    explicit McpClientManager(std::string cache_dir);
    ~McpClientManager();

    McpClientManager(const McpClientManager&) = delete;
    McpClientManager& operator=(const McpClientManager&) = delete;

    void register_routes(httplib::Server& server);
    void stop_all();

    static McpServerConfig parse_server_config_json(const json& value,
                                                     bool allow_missing_id = false);
    static json config_to_json(const McpServerConfig& config,
                               bool include_env_values = false);
    static std::string make_chat_tool_name(const std::string& server_id,
                                           const std::string& tool_name);

    json list_servers_json() const;
    json list_tools_json() const;
    json upsert_server_json(const json& body);
    json remove_server_json(const std::string& id);
    json connect_server_json(const std::string& id);
    json disconnect_server_json(const std::string& id);
    json refresh_tools_json(const std::string& id);
    json call_tool_json(const std::string& id, const json& body);

private:
    struct Runtime;

    std::shared_ptr<Runtime> get_or_create_runtime(const McpServerConfig& config);
    McpServerConfig config_for_id(const std::string& id) const;

    void load_config_file();
    void save_config_file_locked(
        const std::map<std::string, McpServerConfig>& configs) const;
    std::string next_id_locked(const std::string& seed) const;

    std::string cache_dir_;
    std::string config_path_;

    mutable std::mutex mutex_;
    std::map<std::string, McpServerConfig> configs_;
    std::map<std::string, std::shared_ptr<Runtime>> runtimes_;
};

void register_mcp_client_routes(httplib::Server& server,
                                const std::string& cache_dir);

}  // namespace lemon
