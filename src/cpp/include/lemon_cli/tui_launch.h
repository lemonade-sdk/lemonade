#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <string>

namespace lemon_cli {

struct LaunchTuiState {
    std::string agent;
    std::string model;
    std::string agent_args;
    std::string codex_model_provider = "lemonade";
    bool codex_use_user_config = false;
    std::string recipe_dir;
    std::string recipe_file;
    nlohmann::json recipe_options = nlohmann::json::object();
};

bool launch_tui(lemonade::LemonadeClient& client, LaunchTuiState& state);

}  // namespace lemon_cli
