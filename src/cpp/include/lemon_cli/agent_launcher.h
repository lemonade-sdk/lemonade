#pragma once

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

// Build launcher configuration for a supported agent.
// Returns true on success, false if agent is unknown.
bool build_agent_config(const std::string& agent,
                        const std::string& host,
                        int port,
                        const std::string& model,
                        AgentConfig& config,
                        std::string& error_message);

// Locate the agent executable.
// Returns absolute or PATH-resolved executable for ProcessManager::start_process.
std::string find_agent_binary(const AgentConfig& config);

} // namespace lemon_tray
