#pragma once

#include <string>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

const AgentConfigProfile& pi_profile();

// Pi reads the default provider/model from ~/.pi/agent/settings.json, which is
// separate from the provider definitions in models.json. Write it explicitly so
// pi launches straight into the selected local model.
bool sync_pi_settings_file(const std::string& provider_name,
                           const std::string& default_model,
                           std::string& error_out);

} // namespace lemon_cli
