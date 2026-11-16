// Checks the JSON `lemonade launch pi` writes into pi's agent directory: the
// provider block in models.json and the "lemonade" server in mcp.json.
// Build with: cmake --build --preset default --target test_pi_profile
// Run with: ctest --test-dir build -R '^PiProfileTest$' --output-on-failure

#include "lemon_cli/agent_launcher.h"
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
        {"maxTokensField", "max_tokens"},
    });
    check("plain model", provider["models"][0] == json{
        {"id", "Plain"}, {"contextWindow", 32768}, {"maxTokens", 32768}, {"input", {"text"}},
    });
    check("vision model takes images", provider["models"][1]["input"] == json{"text", "image"});
    check("reasoning model leaves thinking to the server", provider["models"][2] == json{
        {"id", "Thinker"}, {"contextWindow", 40960}, {"maxTokens", 40960}, {"input", {"text"}},
    });
    check("unknown window is left to pi", provider["models"][3] == json{
        {"id", "Unknown"}, {"input", {"text"}},
    });
}

void test_api_key() {
    const auto api_key_for = [](const std::string& key) {
        return lemon_cli::pi_profile().build_provider_block(
            "http://localhost:13305/v1", key, {{"Plain", "", 32768, {"chat"}}})["apiKey"];
    };
    check("no key writes the placeholder", api_key_for("") == "lemonade");
    check("a key is referenced from the environment, never written",
          api_key_for("real-secret-key") == "${LEMONADE_API_KEY}");
}

void test_server_origin() {
    check("https server gets an https origin",
          lemon_tray::build_agent_server_base_url("my.server", 443, true) ==
              "https://my.server:443");
    check("wildcard host keeps the scheme",
          lemon_tray::build_agent_server_base_url("0.0.0.0", 13305, true) ==
              "https://localhost:13305");
    check("plain server stays http",
          lemon_tray::build_agent_server_base_url("localhost", 13305, false) ==
              "http://localhost:13305");
}

void test_pi_version() {
    const auto parsed = [](const std::string& output, int major, int minor) {
        int found_major = -1;
        int found_minor = -1;
        return lemon_cli::parse_pi_version(output, found_major, found_minor) &&
               found_major == major && found_minor == minor;
    };
    check("pi --version output is parsed", parsed("1.1.0\n", 1, 1));
    check("an older pi version is parsed", parsed("0.99.2", 0, 99));
    check("a prefixed version is parsed", parsed("pi v1.0.4", 1, 0));
    int major = 0;
    int minor = 0;
    check("output without a version is not parsed",
          !lemon_cli::parse_pi_version("fake-agent-ok\n", major, minor) &&
              !lemon_cli::parse_pi_version("", major, minor));
}

void test_mcp_server(const fs::path& mcp_path) {
    std::string error;
    write_file(mcp_path, R"({
        "autoEnableCodemode": false,
        "mcpServers": {
            "other": {"url": "http://other/mcp"},
            "lemonade": {"url": "http://localhost:13305/mcp", "enabled": false,
                         "headers": {"X-Trace": "1"}}
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
                "headers": {"X-Trace": "1", "Authorization": "Bearer ${LEMONADE_API_KEY}"},
                "exposure": "direct"
            }
        }
    })");
    mcp["mcpServers"]["lemonade"].erase("description");
    check("add keeps other servers, top-level keys and user-set keys", mcp == expected);

    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh without a key drops only the key header",
          mcp["mcpServers"]["lemonade"]["headers"] == json{{"X-Trace", "1"}});

    // Credentials meant for the old server must not be sent to the new one.
    mcp["mcpServers"]["lemonade"]["headers"]["Authorization"] = "Bearer token-for-old";
    mcp["mcpServers"]["lemonade"]["oauth"] = {{"clientId", "old"}};
    mcp["mcpServers"]["lemonade"]["auth"] = {{"provider", "old"}};
    write_file(mcp_path, mcp.dump());
    lemon_cli::sync_pi_mcp_server("http://127.0.0.1:9000", false, error);
    mcp = json::parse(read_file(mcp_path));
    const json& moved = mcp["mcpServers"]["lemonade"];
    check("refresh to another server drops the old server's credentials",
          moved["url"] == "http://127.0.0.1:9000/mcp" && !moved.contains("headers") &&
              !moved.contains("oauth") && !moved.contains("auth") &&
              moved["enabled"] == false);

    mcp["mcpServers"]["lemonade"]["headers"] = {{"Authorization", "Bearer token-for-old"},
                                                {"X-API-Key", "key-for-old"}};
    write_file(mcp_path, mcp.dump());
    lemon_cli::sync_pi_mcp_server("https://other.example:443", true, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh to another server with a key sends only the key reference",
          mcp["mcpServers"]["lemonade"]["headers"] ==
              json{{"Authorization", "Bearer ${LEMONADE_API_KEY}"}});

    lemon_cli::remove_pi_mcp_server(error);
    mcp = json::parse(read_file(mcp_path));
    check("remove drops only the lemonade server",
          mcp == json::parse(R"({"autoEnableCodemode": false,
                                 "mcpServers": {"other": {"url": "http://other/mcp"}}})"));

    // Pi rejects an entry whose type is not an HTTP one even when it has a url.
    write_file(mcp_path, R"({"mcpServers": {"lemonade": {
        "type": "stdio", "command": "npx", "args": ["lemonade-mcp"],
        "env": {"A": "1"}, "cwd": "/tmp", "timeout": 30
    }}})");
    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    mcp = json::parse(read_file(mcp_path));
    const json& migrated = mcp["mcpServers"]["lemonade"];
    check("refresh turns a stdio entry into an HTTP one and keeps user settings",
          migrated["url"] == "http://localhost:13305/mcp" && !migrated.contains("type") &&
              !migrated.contains("command") && !migrated.contains("args") &&
              !migrated.contains("env") && !migrated.contains("cwd") &&
              migrated["timeout"] == 30);

    write_file(mcp_path, R"({"mcpServers": {"lemonade": {
        "type": "streamable-http", "url": "http://old:1/mcp"
    }}})");
    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh keeps an HTTP type the user set",
          mcp["mcpServers"]["lemonade"]["type"] == "streamable-http");

    // Only the exact url launch writes counts as the same server.
    write_file(mcp_path, R"({"mcpServers": {"lemonade": {
        "url": "http://localhost:13305/mcp/", "headers": {"X-Trace": "1"}
    }}})");
    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh treats another form of the url as another server",
          !mcp["mcpServers"]["lemonade"].contains("headers"));

    write_file(mcp_path, R"({"mcpServers": {"lemonade": {
        "url": "http://localhost:13305/mcp",
        "headers": {"Authorization": "Bearer my-own-token"}
    }}})");
    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    mcp = json::parse(read_file(mcp_path));
    check("refresh to the same server without a key keeps the user's Authorization",
          mcp["mcpServers"]["lemonade"]["headers"] ==
              json{{"Authorization", "Bearer my-own-token"}});

    write_file(mcp_path, "{ not json");
    check("malformed mcp.json is refused and left untouched",
          !lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error) &&
              read_file(mcp_path) == "{ not json");
}

#ifndef _WIN32
// mcp.json can hold other servers' tokens, so rewriting it must not widen its
// permissions or replace a symlinked file with a regular one.
void test_mcp_file_kept(const fs::path& agent_dir) {
    std::string error;
    const fs::path mcp_path = agent_dir / "mcp.json";
    const std::string content =
        R"({"mcpServers": {"other": {"url": "https://x/mcp",
                                     "headers": {"Authorization": "Bearer secret"}},
                           "lemonade": {"url": "http://localhost:13305/mcp"}}})";

    write_file(mcp_path, content);
    fs::permissions(mcp_path, fs::perms::owner_read | fs::perms::owner_write);
    lemon_cli::sync_pi_mcp_server("http://localhost:13305", false, error);
    check("refresh keeps a private mcp.json private",
          fs::status(mcp_path).permissions() ==
              (fs::perms::owner_read | fs::perms::owner_write));

    write_file(mcp_path, content);
    fs::permissions(mcp_path, fs::perms::owner_read | fs::perms::owner_write);
    lemon_cli::remove_pi_mcp_server(error);
    check("--no-mcp removal keeps a private mcp.json private",
          fs::status(mcp_path).permissions() ==
              (fs::perms::owner_read | fs::perms::owner_write));

    const fs::path target = agent_dir / "dotfiles-mcp.json";
    fs::remove(mcp_path);
    write_file(target, content);
    fs::create_symlink(target, mcp_path);
    lemon_cli::sync_pi_mcp_server("http://127.0.0.1:9000", false, error);
    check("refresh writes through a symlinked mcp.json",
          fs::is_symlink(mcp_path) &&
              json::parse(read_file(target))["mcpServers"]["lemonade"]["url"] ==
                  "http://127.0.0.1:9000/mcp");
    fs::remove(mcp_path);
}
#endif

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
    test_api_key();
    test_server_origin();
    test_pi_version();
    test_mcp_server(agent_dir / "mcp.json");
#ifndef _WIN32
    test_mcp_file_kept(agent_dir);
#endif

    std::error_code ec;
    fs::remove_all(agent_dir, ec);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
