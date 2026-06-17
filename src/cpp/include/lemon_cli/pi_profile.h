#pragma once

#include <string>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& pi_profile();

// Check if pi already has a defaultProvider and defaultModel configured.
// Returns true if both are set, false otherwise.
bool pi_has_default_config();

// Pi reads the default provider/model from ~/.pi/agent/settings.json, which is
// separate from the provider definitions in models.json. Write it to set the
// default provider and model for pi.
bool sync_pi_settings_file(const std::string& provider_name,
                           const std::string& default_model,
                           std::string& error_out);

} // namespace lemon_cli
