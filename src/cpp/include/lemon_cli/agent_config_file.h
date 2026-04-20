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

bool sync_agent_config_file(const AgentConfigProfile& profile,
                            const std::string& provider_name,
                            const std::string& base_url,
                            const std::string& api_key,
                            const std::vector<AgentModelEntry>& models,
                            std::string& error_out);

} // namespace lemon_cli
