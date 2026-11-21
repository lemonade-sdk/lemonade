#pragma once

#include "lemon_cli/lemonade_client.h"

#include <optional>
#include <string>

namespace lemon_cli {

struct ChatOptions {
    std::string initial_model;
    std::string system_prompt;
    bool stream = true;
    std::optional<std::string> prompt;
    bool json_output = false;
};

int run_chat_repl(lemonade::LemonadeClient& client, const ChatOptions& options);

} // namespace lemon_cli
