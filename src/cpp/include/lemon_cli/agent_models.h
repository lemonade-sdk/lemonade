#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon_cli/agent_config_file.h"

namespace lemon_cli {

// what() is the server's error message, not the raw body.
struct LemondHttpError : std::runtime_error {
    LemondHttpError(int status, const std::string& message)
        : std::runtime_error(message), status(status) {}
    int status;
};

// Calls a lemond API path such as "/api/v1/models" and returns the response
// body. Throws LemondHttpError for an HTTP error status.
using LemondCall = std::function<std::string(const std::string& method,
                                             const std::string& path,
                                             const std::string& body,
                                             int timeout_ms)>;

enum class PrepLevel { Status, Warning, Error };
using PrepReport = std::function<void(PrepLevel, const std::string&)>;

AgentModelEntry agent_model_entry_from_json(const std::string& id,
                                            const nlohmann::json& model,
                                            int default_context_window);

// Downloaded models labeled chat. A failure yields an empty list and, when
// error_out is given, the reason.
std::vector<AgentModelEntry> fetch_llm_models_for_sync(const LemondCall& call,
                                                       int context_window,
                                                       std::string* error_out = nullptr);

// Auto-tune sizes the context only when the model loads, so the window is
// read from the server after loading. Returns false when the load fails;
// context_window stays 0 when the size can't be read.
bool load_for_context_window(const LemondCall& call,
                             const std::string& model,
                             const nlohmann::json& recipe_options,
                             const PrepReport& report,
                             int& context_window);

} // namespace lemon_cli
