#pragma once

#include "lemon_cli/lemonade_client.h"

#include <string>

namespace lemon_cli {

// Prompt the user to select an available model when none was provided.
// Returns true on success and writes the selected model into model_out.
bool resolve_model_if_missing(lemonade::LemonadeClient& client,
                              std::string& model_out,
                              const std::string& command_name,
                              bool show_all = true,
                              const std::string& agent_name = "");

// Prompt the user for a yes/no answer.
bool prompt_yes_no(const std::string& prompt, bool default_yes = false);

} // namespace lemon_cli
