#pragma once

#include "lemon_cli/lemonade_client.h"

#include <nlohmann/json.hpp>

#include <string>

namespace lemon_cli {

struct PullTuiResult {
    nlohmann::json request = nlohmann::json::object();
    std::string display_name;
};

bool pull_tui(lemonade::LemonadeClient& client,
              const std::string& initial_model,
              PullTuiResult& result);

int pull_progress_tui(lemonade::LemonadeClient& client,
                      const PullTuiResult& pull,
                      bool upgrade);

}  // namespace lemon_cli
