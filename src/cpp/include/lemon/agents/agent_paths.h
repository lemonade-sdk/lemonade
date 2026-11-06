#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace lemon::agents {

// Where the agent host keeps payloads, shims and its own state. Each OS has
// its own table; see docs/guide/agents.md.
struct AgentPaths {
    std::filesystem::path data_root;  // <data>/lemonade/agents; payloads in <id>/<version>/
    std::filesystem::path bin_dir;    // shims
    std::filesystem::path state_dir;  // token, locks, log, host.json

    std::filesystem::path agent_dir(const std::string& id) const { return data_root / id; }
    std::filesystem::path token_file() const { return state_dir / "token"; }
    std::filesystem::path host_lock() const { return state_dir / "host.lock"; }
    std::filesystem::path takeover_lock() const { return state_dir / "takeover.lock"; }
    std::filesystem::path host_info() const { return state_dir / "host.json"; }
    std::filesystem::path log_file() const { return state_dir / "agent-host.log"; }
};

using EnvLookup = std::function<std::string(const std::string& name)>;

AgentPaths agent_paths_from(const EnvLookup& env);
AgentPaths agent_paths();

} // namespace lemon::agents
