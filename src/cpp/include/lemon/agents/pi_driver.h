#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/agents/agent_paths.h"
#include "lemon/utils/http_security_policy.h"
#include "lemon_cli/agent_models.h"

namespace lemon::agents {

// State every host request shares.
struct HostContext {
    AgentPaths paths;
    std::string lemond_origin;  // e.g. http://127.0.0.1:13305
    std::string api_key;        // presented to lemond; empty when it has none
    std::string token;
    std::atomic<bool> auth_required{false};
    std::string release_base_url;
    lemon::utils::HttpSecurityPolicy download_policy =
        lemon::utils::HttpSecurityPolicy::ExternalHttpsOnly;
    // Held only while pi's config files are written.
    std::mutex write_mutex;

    // Version directories a launch picked, kept by installs until the client
    // has had time to run them.
    void lease(const std::filesystem::path& dir, std::chrono::steady_clock::time_point until);
    bool leased(const std::filesystem::path& dir);

private:
    std::mutex lease_mutex_;
    std::map<std::filesystem::path, std::chrono::steady_clock::time_point> leases_;
};

lemon_cli::LemondCall make_lemond_call(const std::string& origin, const std::string& api_key);

using EventSink = std::function<void(const std::string& event, const nlohmann::json& data)>;

// The command a launch spec describes, before it is handed out.
struct AgentCommand {
    std::string binary;
    std::vector<std::string> args;
    std::string cwd;
    nlohmann::json env = nlohmann::json::object();
};

// The seam for running agents under a sandbox later; phase 1 changes nothing.
AgentCommand apply_execution_policy(AgentCommand command);

// The first pi on search_path, then at pi's usual install locations, that
// isn't Lemonade's shim. Empty when there is none.
std::string find_external_pi(const std::string& search_path);

// The request's agent_dir, which must be absolute: the host's own working
// directory means nothing to the client. Empty when the request has none.
std::string requested_agent_dir(const nlohmann::json& body);

nlohmann::json pi_status(HostContext& ctx);

// Writes pi's provider and MCP entry for lemond. Returns warnings; throws
// AgentError 500 config_write_failed.
std::vector<std::string> pi_configure(HostContext& ctx, std::optional<bool> mcp,
                                      const std::string& agent_dir);

// Prepares pi for the request and returns the complete event's data. Throws AgentError.
nlohmann::json pi_launch(HostContext& ctx, const nlohmann::json& body, const EventSink& events,
                         const std::function<bool()>& installing);

} // namespace lemon::agents
