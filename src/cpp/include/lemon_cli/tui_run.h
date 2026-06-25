#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lemon_cli {

struct RunTuiState {
    std::string model;
    nlohmann::json recipe_options = nlohmann::json::object();
    bool save_options = false;
    std::optional<bool> pinned = std::nullopt;
    bool chat_cli = false;
};

bool run_tui(lemonade::LemonadeClient& client, RunTuiState& state);

}  // namespace lemon_cli
