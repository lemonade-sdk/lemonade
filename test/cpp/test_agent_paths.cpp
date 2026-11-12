// Where the agent host keeps payloads, shims and state on each OS.
#include "agent_test_util.h"

#include <map>

#include "lemon/agents/agent_paths.h"

using agent_test::check;
namespace fs = std::filesystem;

int main() {
    auto env_of = [](std::map<std::string, std::string> values) {
        return [values](const std::string& name) {
            auto it = values.find(name);
            return it == values.end() ? std::string() : it->second;
        };
    };
#if defined(_WIN32)
    const auto paths = lemon::agents::agent_paths_from(
        env_of({{"LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"}, {"USERPROFILE", "C:\\Users\\u"}}));
    check("payloads under LOCALAPPDATA",
          paths.data_root == fs::path("C:\\Users\\u\\AppData\\Local\\lemonade\\agents"));
    check("shims under USERPROFILE\\.local\\bin", paths.bin_dir == fs::path("C:\\Users\\u\\.local\\bin"));
    check("state under LOCALAPPDATA",
          paths.state_dir == fs::path("C:\\Users\\u\\AppData\\Local\\lemonade\\agent-host"));
#elif defined(__APPLE__)
    const auto paths = lemon::agents::agent_paths_from(env_of({{"HOME", "/Users/u"}}));
    check("payloads under Application Support",
          paths.data_root == fs::path("/Users/u/Library/Application Support/lemonade/agents"));
    check("shims in ~/.local/bin", paths.bin_dir == fs::path("/Users/u/.local/bin"));
    check("state under Application Support",
          paths.state_dir == fs::path("/Users/u/Library/Application Support/lemonade/agent-host"));
#else
    const auto defaults = lemon::agents::agent_paths_from(env_of({{"HOME", "/home/u"}}));
    check("default data dir", defaults.data_root == fs::path("/home/u/.local/share/lemonade/agents"));
    check("default bin dir", defaults.bin_dir == fs::path("/home/u/.local/bin"));
    check("default state dir",
          defaults.state_dir == fs::path("/home/u/.local/state/lemonade/agent-host"));
    check("token file in the state dir",
          defaults.token_file() == fs::path("/home/u/.local/state/lemonade/agent-host/token"));

    const auto overridden = lemon::agents::agent_paths_from(
        env_of({{"HOME", "/home/u"}, {"XDG_DATA_HOME", "/d"}, {"XDG_STATE_HOME", "/s"},
                {"XDG_BIN_HOME", "/b"}}));
    check("XDG_DATA_HOME", overridden.data_root == fs::path("/d/lemonade/agents"));
    check("XDG_STATE_HOME", overridden.state_dir == fs::path("/s/lemonade/agent-host"));
    check("XDG_BIN_HOME", overridden.bin_dir == fs::path("/b"));

    const auto relative = lemon::agents::agent_paths_from(
        env_of({{"HOME", "/home/u"}, {"XDG_DATA_HOME", "rel"}}));
    check("relative XDG values are ignored",
          relative.data_root == fs::path("/home/u/.local/share/lemonade/agents"));
#endif
    return agent_test::finish();
}
