#pragma once

#include "lemon_cli/lemonade_client.h"

#include <string>

namespace lemon_cli {

// Accept either a bare Hugging Face repo id / checkpoint string or a full
// huggingface.co URL and return the canonical checkpoint form expected by the
// CLI and server (for example: owner/repo[:variant]).
std::string normalize_huggingface_checkpoint_arg(const std::string& arg);

// Pull a model from a Hugging Face checkpoint id, optionally with a variant
// suffix (`owner/repo[:variant]`). Fetches /api/v1/pull/variants from lemond,
// presents an interactive variant menu when needed, then issues /v1/pull.
//
// `assume_yes` skips the interactive menu and picks the first variant.
// Returns 0 on success, non-zero on error.
int hf_pull_flow(lemonade::LemonadeClient& client,
                 const std::string& model_arg,
                 bool assume_yes);

}  // namespace lemon_cli
