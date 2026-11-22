#pragma once

#include <lemon_cli/agent_launcher.h>
#include <ostream>
#include <string>

namespace lemon_cli {

// The implementation is selected by CMake, like the existing platform sources.
void print_manual_shell_recipe(const lemon_tray::AgentConfig& agent_config,
                               const std::string& api_key,
                               const std::string& key_env,
                               std::ostream& out);

} // namespace lemon_cli
