#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "lemon/agents/agent_paths.h"
#include "lemon/agents/agent_pins.h"
#include "lemon/agents/agent_shim.h"
#include "lemon/utils/cancel_token.h"
#include "lemon/utils/http_security_policy.h"

namespace lemon::agents {

struct InstalledAgent {
    std::string version;
    std::filesystem::path dir;
    std::filesystem::path binary;
};

// The committed install of `version`, or with an empty version the newest
// committed one. Only directories with a matching manifest count.
std::optional<InstalledAgent> find_installed(const AgentDescriptor& agent,
                                             const AgentPaths& paths,
                                             const std::string& version = "");

struct InstallOutcome {
    InstalledAgent installed;
    bool downloaded = false;
    ShimState shim = ShimState::Absent;
    std::vector<std::string> warnings;
};

using InstallProgress = std::function<bool(size_t downloaded, size_t total)>;

// Installs the pinned payload and points the shim at it. Throws AgentError:
// 502 download_failed / checksum_mismatch, 500 install_failed, 409 agent_busy
// when Windows keeps the old files open.
InstallOutcome install_agent(const AgentDescriptor& agent, const AgentPin& pin,
                             const AgentPaths& paths, bool force,
                             const InstallProgress& progress,
                             const lemon::utils::CancelToken& cancel,
                             lemon::utils::HttpSecurityPolicy policy =
                                 lemon::utils::HttpSecurityPolicy::ExternalHttpsOnly);

// Removes every payload directory and our shim. Files Windows keeps open stay
// behind with a warning and are retried by the next install. Returns whether
// anything was removed.
bool uninstall_agent(const AgentDescriptor& agent, const AgentPaths& paths,
                     std::vector<std::string>& warnings);

} // namespace lemon::agents
