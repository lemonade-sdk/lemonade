#pragma once

#include <string>

namespace lemon_cli {

// Junie reads custom model profiles from standalone JSON files under
// ~/.junie/models/<id>.json (or $JUNIE_HOME/models/<id>.json when JUNIE_HOME is
// set). Unlike opencode/pi, there is no single config file with a providers map:
// each file *is* one model profile. Write ~/.junie/models/lemonade.json so that
// `junie --model custom:lemonade` points Junie at the local Lemonade server.
bool sync_junie_model_file(const std::string& base_url,
                           const std::string& api_key,
                           const std::string& model_id,
                           std::string& error_out);

} // namespace lemon_cli
