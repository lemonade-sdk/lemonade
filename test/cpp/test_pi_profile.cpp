// Checks the JSON `lemonade launch pi` writes into pi's agent directory: the
// provider block in models.json and the "lemonade" server in mcp.json.
// Build with: cmake --build --preset default --target test_pi_profile
// Run with: ctest --test-dir build -R '^PiProfileTest$' --output-on-failure

#include "lemon_cli/pi_profile.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

void test_provider_block() {
    const json provider = lemon_cli::pi_profile().build_provider_block(
        "http://localhost:13305/v1", "",
        {
            {"Plain", "", 32768, {"chat"}},
            {"Vision", "", 65536, {"chat", "vision"}},
            {"Thinker", "", 40960, {"chat", "reasoning"}},
            {"Unknown", "", 0, {"chat"}},
        });

    check("provider compat targets llama.cpp", provider["compat"] == json{
        {"supportsStore", false},
        {"supportsDeveloperRole", false},
        {"supportsReasoningEffort", false},
        {"maxTokensField", "max_tokens"},
    });
    check("plain model", provider["models"][0] == json{
        {"id", "Plain"}, {"contextWindow", 32768}, {"maxTokens", 32768}, {"input", {"text"}},
    });
    check("vision model takes images", provider["models"][1]["input"] == json{"text", "image"});
    check("reasoning model toggles thinking off and on", provider["models"][2] == json{
        {"id", "Thinker"}, {"contextWindow", 40960}, {"maxTokens", 40960}, {"input", {"text"}},
        {"reasoning", true},
        {"thinkingLevelMap", {{"off", "off"}, {"minimal", nullptr}, {"low", nullptr},
                              {"medium", "medium"}, {"high", nullptr}, {"xhigh", nullptr}}},
        {"compat", {{"thinkingFormat", "qwen-chat-template"}}},
    });
    check("unknown window is left to pi", provider["models"][3] == json{
        {"id", "Unknown"}, {"input", {"text"}},
    });
}

void test_mcp_server(const fs::path& mcp_path) {
    std::string error;
    write_file(mcp_path, R"({
        "autoEnableCodemode": false,
        "mcpServers": {
            "other": {"url": "http://other/mcp"},
            "lemonade": {"url": "http://old:1/mcp", "enabled": false}
        }
    })");

    lemon_cli::sync_pi_mcp_server("http://localhost:13305", true, error);
    json mcp = json::parse(read_file(mcp_path));
    json expected = json::parse(R"({
        "autoEnableCodemode": false,
        "mcpServers": {
            "other": {"url": "http://other/mcp"},
            "lemonade": {
                "url": "http://localhost:13305/mcp",
                "enabled": false,
                "headers": {"Authorization": "Bearer ${LEMONADE_API_KEY}"},
                "exposure": "direct"
            }
        }
    })");
    mcp["mcpServers"]["lemonade"].erase("description");
    check("add keeps other servers, top-level keys and user-set keys", mcp == expected);

    lemon_cli::sync_pi_mcp_server("http://127.0.0.1:9000", false, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh follows the origin and drops the header without a key",
          mcp["mcpServers"]["lemonade"]["url"] == "http://127.0.0.1:9000/mcp" &&
              !mcp["mcpServers"]["lemonade"].contains("headers"));

    lemon_cli::remove_pi_mcp_server(error);
    mcp = json::parse(read_file(mcp_path));
    check("remove drops only the lemonade server",
          mcp == json::parse(R"({"autoEnableCodemode": false,
                                 "mcpServers": {"other": {"url": "http://other/mcp"}}})"));

    write_file(mcp_path, "{ not json");
    check("malformed mcp.json is refused and left untouched",
          !lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error) &&
              read_file(mcp_path) == "{ not json");
}

}  // namespace

int main() {
    const fs::path agent_dir = fs::temp_directory_path() /
        ("lemonade-pi-profile-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(agent_dir);
#ifdef _WIN32
    _putenv_s("PI_CODING_AGENT_DIR", agent_dir.string().c_str());
#else
    setenv("PI_CODING_AGENT_DIR", agent_dir.string().c_str(), 1);
#endif

    test_provider_block();
    test_mcp_server(agent_dir / "mcp.json");

    std::error_code ec;
    fs::remove_all(agent_dir, ec);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
