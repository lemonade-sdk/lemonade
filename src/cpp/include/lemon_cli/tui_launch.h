#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lemon_cli {

struct LaunchTuiState {
    std::vector<std::string> agents;
    std::string host;
    int port = 0;
    bool is_ssl = false;
    std::string agent;
    std::string model;
    std::string agent_args;
    std::string codex_model_provider = "lemonade";
    bool codex_use_user_config = false;
    std::optional<bool> pi_mcp;
    bool pi_codemode = false;
    std::string recipe_dir;
    std::string recipe_file;
    nlohmann::json recipe_options = nlohmann::json::object();
    nlohmann::json preset_payload;
    bool download_required = false;
};

bool launch_tui(lemonade::LemonadeClient& client, LaunchTuiState& state);

}  // namespace lemon_cli
