#include "lemon/agents/agent_paths.h"

#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

namespace lemon::agents {

AgentPaths agent_paths_from(const EnvLookup& env) {
    using lemon::utils::path_from_utf8;
    AgentPaths paths;
#ifdef _WIN32
    const fs::path local = path_from_utf8(env("LOCALAPPDATA")) / "lemonade";
    paths.data_root = local / "agents";
    paths.state_dir = local / "agent-host";
    paths.bin_dir = path_from_utf8(env("USERPROFILE")) / ".local" / "bin";
#else
    const fs::path home = path_from_utf8(env("HOME"));
#ifdef __APPLE__
    const fs::path support = home / "Library" / "Application Support" / "lemonade";
    paths.data_root = support / "agents";
    paths.state_dir = support / "agent-host";
    paths.bin_dir = home / ".local" / "bin";
#else
    auto xdg = [&](const char* name, const fs::path& fallback) {
        const std::string value = env(name);
        // XDG says relative values are invalid and must be ignored.
        return !value.empty() && path_from_utf8(value).is_absolute() ? path_from_utf8(value)
                                                                      : fallback;
    };
    paths.data_root = xdg("XDG_DATA_HOME", home / ".local" / "share") / "lemonade" / "agents";
    paths.state_dir = xdg("XDG_STATE_HOME", home / ".local" / "state") / "lemonade" / "agent-host";
    paths.bin_dir = xdg("XDG_BIN_HOME", home / ".local" / "bin");
#endif
#endif
    return paths;
}

AgentPaths agent_paths() {
    return agent_paths_from(
        [](const std::string& name) { return lemon::utils::get_environment_variable_utf8(name); });
}

} // namespace lemon::agents
