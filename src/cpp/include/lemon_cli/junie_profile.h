#pragma once

#include <string>

namespace lemon_cli {

// Writes <junie home>/models/lemonade.json, whose file name is what
// `--model custom:lemonade` resolves. base_url must be the full chat
// completions endpoint: Junie appends nothing to it.
bool sync_junie_model_file(const std::string& base_url,
                           const std::string& api_key,
                           const std::string& model_id,
                           int context_window,
                           std::string& error_out);

} // namespace lemon_cli
