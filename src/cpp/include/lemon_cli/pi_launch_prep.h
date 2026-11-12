#pragma once

#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon_cli/agent_models.h"

namespace lemon_cli {

struct PiPrepRequest {
    // nullopt: configure only, with no selected-model lookup, load or settings write.
    std::optional<std::string> model;
    // pi's baseUrl is <origin>/v1 and its MCP entry <origin>/mcp.
    std::string server_origin;
    // Whether pi gets LEMONADE_API_KEY in its environment.
    bool has_api_key = false;
    std::optional<bool> mcp;
    nlohmann::json recipe_options = nlohmann::json::object();
    // pi's home; empty resolves PI_CODING_AGENT_DIR or ~/.pi/agent from this process.
    std::string agent_dir;
};

// Writes pi's Lemonade provider, MCP entry and (with a model, when pi has no
// default) its default model. Every fetch and load happens before the writes;
// write_mutex, when given, is held only for the writes. Returns false when pi
// must not be started. model_loaded is set when the selected model was loaded.
bool prepare_pi_config(const LemondCall& call,
                       const PiPrepRequest& request,
                       const PrepReport& report,
                       bool* model_loaded = nullptr,
                       std::mutex* write_mutex = nullptr);

// Older pi starts fine with these flags but without what they enable, so the
// version is checked only when one is passed. Returns false when pi must not
// be started.
bool check_pi_version_for_flags(const std::string& pi_binary,
                                bool codemode,
                                bool mcp,
                                const std::string& install_instructions,
                                const PrepReport& report);

} // namespace lemon_cli
