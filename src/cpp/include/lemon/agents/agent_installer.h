#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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

// The install launch uses: the pinned version when it is installed, else the
// newest. An install keeps the version it replaced, which can be newer.
std::optional<InstalledAgent> find_active(const AgentDescriptor& agent, const AgentPaths& paths,
                                          const std::string& pinned_version);

// Versions launches have picked. A launch picks and acquires under mutex(),
// and an install retires old versions under it, so neither sees the other
// half done. A version stays held while any launch uses it and for a grace
// period after the last one, for the client to start it.
class VersionLeases {
public:
    std::mutex& mutex() { return mutex_; }
    void acquire(const std::filesystem::path& dir);  // with mutex() held
    bool held(const std::filesystem::path& dir);     // with mutex() held
    void release(const std::filesystem::path& dir, std::chrono::seconds grace);

private:
    struct Lease {
        int launches = 0;
        std::chrono::steady_clock::time_point until{};
    };
    std::mutex mutex_;
    std::map<std::filesystem::path, Lease> leases_;
};

struct InstallOutcome {
    InstalledAgent installed;
    bool downloaded = false;
    ShimState shim = ShimState::Absent;
    std::vector<std::string> warnings;
};

using InstallProgress = std::function<bool(size_t downloaded, size_t total)>;

// Installs the pinned payload and points the shim at it. Older versions are
// removed unless a process runs from one, the shim still names it, or a
// launch holds it in leases. Throws AgentError:
// 502 download_failed / checksum_mismatch, 500 install_failed, 409 agent_busy
// when Windows keeps the old files open.
InstallOutcome install_agent(const AgentDescriptor& agent, const AgentPin& pin,
                             const AgentPaths& paths, bool force,
                             const InstallProgress& progress,
                             const lemon::utils::CancelToken& cancel,
                             lemon::utils::HttpSecurityPolicy policy =
                                 lemon::utils::HttpSecurityPolicy::ExternalHttpsOnly,
                             VersionLeases* leases = nullptr);

// Removes every payload directory and our shim. Files Windows keeps open stay
// behind with a warning and are retried by the next install. Returns whether
// anything was removed.
bool uninstall_agent(const AgentDescriptor& agent, const AgentPaths& paths,
                     std::vector<std::string>& warnings);

} // namespace lemon::agents
