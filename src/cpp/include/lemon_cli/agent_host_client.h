#pragma once

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon_cli/lemonade_client.h"

namespace lemon_cli {

// The loopback address the agent host should dial for a CLI target, or
// nullopt when the target isn't this machine. 0.0.0.0 and localhost map to
// 127.0.0.1, as LemonadeClient does.
std::optional<std::string> loopback_target(const std::string& host);

// Starts exe in its own session with no terminal, so it outlives the CLI.
bool spawn_detached(const std::string& exe, const std::vector<std::string>& args);

struct AgentHostTarget {
    std::string host;
    int port = 0;
    bool is_ssl = false;
    std::string api_key;
};

// Makes sure an agent host serves the CLI's lemond and returns its token.
// Never reads the token for a remote target. Throws std::runtime_error with
// a message for the user.
std::string ensure_agent_host(const lemonade::LemonadeClient& client,
                              const AgentHostTarget& target);

// An HTTP error status from an agent route, with the body's code.
struct AgentRouteError : std::runtime_error {
    AgentRouteError(int status, std::string code, const std::string& message,
                    nlohmann::json body)
        : std::runtime_error(message), status(status), code(std::move(code)),
          body(std::move(body)) {}
    int status;
    std::string code;
    nlohmann::json body;
};

nlohmann::json agent_request(const lemonade::LemonadeClient& client, const std::string& token,
                             const std::string& method, const std::string& path,
                             const nlohmann::json& body = nullptr);

// Streams an agent route and returns the complete event's data. Throws
// AgentRouteError for an error event or an HTTP error.
nlohmann::json agent_stream(
    const lemonade::LemonadeClient& client, const std::string& token, const std::string& path,
    const nlohmann::json& body,
    const std::function<void(const std::string& event, const nlohmann::json& data)>& on_event);

} // namespace lemon_cli
