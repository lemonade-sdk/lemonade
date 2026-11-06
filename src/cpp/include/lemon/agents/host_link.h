#pragma once

#include <chrono>
#include <string>

#include "lemon/agents/host_routes.h"

namespace lemon::agents {

struct LinkOptions {
    std::string ws_url;  // ws://127.0.0.1:13305/v1/agents/host
    bool persistent = false;
    std::chrono::seconds idle_exit{600};
    std::chrono::seconds unreachable_exit{60};
    std::chrono::seconds max_backoff{5};
};

enum class LinkExit { Idle, Unreachable, Unauthorized, TakenOver };

// Keeps the WebSocket to lemond up and serves requests until an on-demand
// host has a reason to exit. A persistent host only returns on a fatal error.
LinkExit run_link(HostContext& ctx, HostRoutes& routes, const LinkOptions& options);

} // namespace lemon::agents
