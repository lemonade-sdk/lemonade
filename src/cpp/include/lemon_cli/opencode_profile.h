#pragma once

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& opencode_profile();

bool is_opencode_v2(const std::string& version);

nlohmann::json build_opencode_v2_config(const std::string& base_url,
                                       const std::string& model,
                                       const std::vector<AgentModelEntry>& models);

} // namespace lemon_cli
