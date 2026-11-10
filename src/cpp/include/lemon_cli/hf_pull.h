#pragma once

#include "lemon_cli/lemonade_client.h"

#include <string>

namespace lemon_cli {

// Accept a bare checkpoint or a Hugging Face / ModelScope model URL. The
// returned value is owner/repo[:variant]. When detected_source is non-null it
// receives "huggingface" or "modelscope" when the URL identifies a provider;
// otherwise it receives the normalized source_hint.
std::string normalize_registry_checkpoint_arg(const std::string& arg,
                                              const std::string& source_hint,
                                              std::string* detected_source = nullptr);

// Backward-compatible helper retained for existing callers/tests.
std::string normalize_huggingface_checkpoint_arg(const std::string& arg);

// Discover variants through lemond and pull from the selected remote registry.
// `recipe_choice` picks the backend when more than one can load the
// repository; empty asks the user.
int registry_pull_flow(lemonade::LemonadeClient& client,
                       const std::string& model_arg,
                       bool assume_yes,
                       const std::string& registry_source,
                       const std::string& recipe_choice = "");

// Backward-compatible Hugging Face entry point.
int hf_pull_flow(lemonade::LemonadeClient& client,
                 const std::string& model_arg,
                 bool assume_yes);

}  // namespace lemon_cli
