#pragma once

#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace lemon_tray {

struct AgentConfig {
    std::string binary_name;
    std::vector<std::string> binary_alternatives;
    std::vector<std::string> fallback_paths;
    std::vector<std::pair<std::string, std::string>> env_vars;
    std::vector<std::string> extra_args;
    std::string install_instructions;
};

struct AgentLaunchOptions {
    bool codex_use_user_config = false;
    std::string codex_model_provider = "lemonade";
};

// Returns true if the agent requires file-based config sync before launch.
bool agent_needs_config_sync(const std::string& agent);

// Build normalized origin for agent endpoints (e.g. http://localhost:13305).
std::string build_agent_server_base_url(const std::string& host, int port, bool is_ssl = false);

// Build launcher configuration for a supported agent.
// Returns true on success, false if agent is unknown.
bool build_agent_config(const std::string& agent,
                        const std::string& host,
                        int port,
                        const std::string& model,
                        const std::string& api_key,
                        const AgentLaunchOptions& launch_options,
                        AgentConfig& config,
                        std::string& error_message);

// OpenCode v2's TUI attaches to a background service that ignores the launch
// environment and caches config, so v2 must run with --standalone. v1 rejects
// that flag. Returns nullopt when the version can't be determined.
std::optional<bool> opencode_needs_standalone(const std::string& binary);

// Adds --standalone where OpenCode v2 accepts it for the given arguments.
void add_opencode_standalone_arg(std::vector<std::string>& args);

// Locate the agent executable.
// Returns absolute or PATH-resolved executable for ProcessManager::start_process.
std::string find_agent_binary(const AgentConfig& config);

} // namespace lemon_tray
