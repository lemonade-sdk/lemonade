// Checks what `lemonade launch opencode` writes into opencode.json (the Lemonade
// provider block and the "lemonade" MCP server) and how it builds the launch.
// Build with: cmake --build --preset default --target test_opencode_profile
// Run with: ctest --test-dir build -R '^OpencodeProfileTest$' --output-on-failure

#include "lemon_cli/agent_launcher.h"
#include "lemon_cli/opencode_profile.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

json build_provider(const std::string& api_key) {
    return lemon_cli::opencode_profile().build_provider_block(
        "http://localhost:13305/v1", api_key,
        {
            {"Plain", "Plain (local)", 32768, {"chat"}},
            {"Vision", "Vision (local)", 65536, {"chat", "vision"}},
            {"Unlabeled", "Unlabeled (local)", 4096, {}},
            {"Unknown", "Unknown (local)", 0, {"chat"}},
        });
}

void test_provider_block() {
    const json models = build_provider("")["models"];
    check("plain model gets its window and text-only input", models["Plain"] == json{
        {"name", "Plain (local)"},
        {"limit", {{"context", 32768}, {"output", 32768 / 3}}},
        {"modalities", {{"input", {"text"}}, {"output", {"text"}}}},
    });
    check("vision model takes images",
          models["Vision"]["modalities"]["input"] == json{"text", "image"});
    check("a model without labels keeps opencode's defaults",
          !models["Unlabeled"].contains("modalities"));
    check("an unknown window is left to opencode", !models["Unknown"].contains("limit"));
    check("tool_call is not written, since it does not disable tools",
          !models["Plain"].contains("tool_call") && !models["Vision"].contains("tool_call"));
}

void test_api_key() {
    check("no key writes the placeholder",
          build_provider("")["options"]["apiKey"] == "lemonade");
    check("a key is referenced from the environment, never written",
          build_provider("real-secret-key")["options"]["apiKey"] == "{env:LEMONADE_API_KEY}");
}

json sync_and_read(const fs::path& config_path, const std::string& origin, bool has_key) {
    std::string error;
    lemon_cli::sync_opencode_mcp_server(origin, has_key, error);
    return json::parse(read_file(config_path));
}

void test_mcp_servers(const fs::path& config_path) {
    write_file(config_path, R"({
        "$schema": "https://opencode.ai/config.json",
        "provider": {"anthropic": {}},
        "mcp": {"servers": {
            "other": {"type": "remote", "url": "http://other/mcp"},
            "lemonade": {"type": "remote", "url": "http://localhost:13305/mcp", "disabled": true,
                         "headers": {"X-Trace": "1"}, "timeout": {"startup": 5000}}
        }}
    })");

    json config = sync_and_read(config_path, "http://localhost:13305", true);
    const json expected = json::parse(R"({
        "$schema": "https://opencode.ai/config.json",
        "provider": {"anthropic": {}},
        "mcp": {"servers": {
            "other": {"type": "remote", "url": "http://other/mcp"},
            "lemonade": {"type": "remote", "url": "http://localhost:13305/mcp", "disabled": true,
                         "headers": {"X-Trace": "1",
                                     "Authorization": "Bearer {env:LEMONADE_API_KEY}"},
                         "timeout": {"startup": 5000}, "oauth": false, "codemode": false}
        }}
    })");
    check("add keeps other servers, top-level keys and user-set keys", config == expected);

    config["mcp"]["servers"]["lemonade"]["codemode"] = true;
    config["mcp"]["servers"]["lemonade"]["oauth"] = {{"clientId", "mine"}};
    write_file(config_path, config.dump());
    config = sync_and_read(config_path, "http://localhost:13305", false);
    const json& kept = config["mcp"]["servers"]["lemonade"];
    check("refresh keeps codemode and oauth the user set", kept["codemode"] == true &&
              kept["oauth"] == json{{"clientId", "mine"}});
    check("refresh without a key drops only the key header",
          kept["headers"] == json{{"X-Trace", "1"}});

    // Credentials meant for the old server must not be sent to the new one.
    config["mcp"]["servers"]["lemonade"]["headers"]["Authorization"] = "Bearer token-for-old";
    config["mcp"]["servers"]["lemonade"]["auth"] = {{"provider", "old"}};
    write_file(config_path, config.dump());
    config = sync_and_read(config_path, "http://127.0.0.1:9000", false);
    const json& moved = config["mcp"]["servers"]["lemonade"];
    check("refresh to another server drops the old server's credentials",
          moved["url"] == "http://127.0.0.1:9000/mcp" && !moved.contains("headers") &&
              !moved.contains("auth") && moved["oauth"] == false);

    write_file(config_path, R"({"mcp": {"servers": {"lemonade": {
        "type": "local", "command": ["npx", "lemonade-mcp"], "environment": {"A": "1"},
        "cwd": "/tmp", "timeout": {"startup": 5000}
    }}}})");
    config = sync_and_read(config_path, "http://localhost:13305", false);
    const json& migrated = config["mcp"]["servers"]["lemonade"];
    check("refresh turns a local entry into a remote one and keeps user settings",
          migrated["type"] == "remote" && migrated["url"] == "http://localhost:13305/mcp" &&
              !migrated.contains("command") && !migrated.contains("environment") &&
              !migrated.contains("cwd") && migrated["timeout"] == json{{"startup", 5000}});

    write_file(config_path, R"({"mcp": {"servers": {"lemonade": {
        "type": "remote", "url": "http://localhost:13305/mcp",
        "headers": {"Authorization": "Bearer my-own-token"}
    }}}})");
    config = sync_and_read(config_path, "http://localhost:13305", false);
    check("refresh to the same server without a key keeps the user's Authorization",
          config["mcp"]["servers"]["lemonade"]["headers"] ==
              json{{"Authorization", "Bearer my-own-token"}});

    write_file(config_path, "{ not json");
    std::string error;
    check("malformed opencode.json is refused and left untouched",
          !lemon_cli::sync_opencode_mcp_server("http://localhost:13305", false, error) &&
              read_file(config_path) == "{ not json");
}

void test_mcp_remove(const fs::path& config_path) {
    write_file(config_path, R"({"mcp": {
        "mine": {"type": "remote", "url": "http://mine/mcp"},
        "servers": {"lemonade": {"type": "remote", "url": "http://b/mcp"},
                    "other": {"type": "remote", "url": "http://other/mcp"}}
    }})");
    check("an entry in mcp.servers.lemonade is detected", lemon_cli::opencode_has_mcp_server());
    std::string error;
    check("remove succeeds", lemon_cli::remove_opencode_mcp_server(error));
    check("remove drops only the lemonade server",
          json::parse(read_file(config_path)) == json::parse(R"({"mcp": {
              "mine": {"type": "remote", "url": "http://mine/mcp"},
              "servers": {"other": {"type": "remote", "url": "http://other/mcp"}}
          }})"));
    check("no entry is detected after remove", !lemon_cli::opencode_has_mcp_server());
}

void test_codemode_config(const fs::path& config_path) {
    write_file(config_path, R"({"mcp": {"servers": {"lemonade": {
        "type": "remote", "url": "http://localhost:13305/mcp", "oauth": false,
        "codemode": false, "timeout": {"startup": 5000}
    }}}})");
    // opencode ignores a partial entry in OPENCODE_CONFIG_CONTENT, so the
    // whole entry is passed with codemode switched on.
    check("codemode config carries the whole entry",
          lemon_cli::opencode_codemode_config() == json::parse(R"({"mcp": {"servers": {
              "lemonade": {"type": "remote", "url": "http://localhost:13305/mcp",
                           "oauth": false, "codemode": true, "timeout": {"startup": 5000}}
          }}})"));

    write_file(config_path, R"({"mcp": {}})");
    check("no codemode config without the entry", lemon_cli::opencode_codemode_config().is_null());
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

void test_version_parse() {
    using lemon_tray::parse_opencode_major_version;
    check("v1 version", parse_opencode_major_version("1.19.0") == 1);
    check("v2 version with prefix", parse_opencode_major_version("opencode v2.0.29") == 2);
    check("non-version output", !parse_opencode_major_version("fake-agent-ok"));
    check("oversized major is unknown, not a crash",
          !parse_opencode_major_version("99999999999.0.0"));
}

void test_standalone_placement() {
    const std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> cases = {
        {{}, {"--standalone"}},
        {{"--auto"}, {"--standalone", "--auto"}},
        {{"./project"}, {"--standalone", "./project"}},
        {{"run", "hello"}, {"run", "--standalone", "hello"}},
        {{"--log-level", "debug", "run", "hi"},
         {"--log-level", "debug", "run", "--standalone", "hi"}},
        {{"debug", "config"}, {"debug", "config"}},
        {{"--server", "http://127.0.0.1:4096"}, {"--server", "http://127.0.0.1:4096"}},
        {{"--standalone"}, {"--standalone"}},
    };
    for (const auto& [args, expected] : cases) {
        std::vector<std::string> actual = args;
        lemon_tray::add_opencode_standalone_arg(actual);
        std::string name = "--standalone placement for [";
        for (const auto& arg : args) name += " " + arg;
        check(name + " ]", actual == expected);
    }
}

std::string inline_config_for(const std::string& existing) {
    set_env("OPENCODE_CONFIG_CONTENT", existing);
    lemon_tray::AgentConfig config;
    std::string error;
    lemon_tray::build_agent_config("opencode", "localhost", 13305, "Qwen", "", {}, config, error);
    set_env("OPENCODE_CONFIG_CONTENT", "");
    for (const auto& [name, value] : config.env_vars) {
        if (name == "OPENCODE_CONFIG_CONTENT") return value;
    }
    return "";
}

void test_inline_config() {
    check("model is selected inline",
          json::parse(inline_config_for("")) == json{{"model", "Lemonade/Qwen"}});
    check("user inline keys survive and the model is overridden",
          json::parse(inline_config_for(R"({"foo": 1, "model": "other/model"})")) ==
              json{{"foo", 1}, {"model", "Lemonade/Qwen"}});
    check("user inline config with comments is merged",
          json::parse(inline_config_for("{ // keep me\n \"foo\": 1 }")) ==
              json{{"foo", 1}, {"model", "Lemonade/Qwen"}});
    // Launch passes the user's own variable through when it isn't set here.
    check("unparsable user inline config is left alone", inline_config_for(R"({"foo": 1,})").empty());
}

#ifndef _WIN32
// opencode.json can hold other providers' keys and other servers' tokens, so
// rewriting it must not widen its permissions or replace a symlinked file.
void test_config_file_kept(const fs::path& config_dir) {
    std::string error;
    const fs::path config_path = config_dir / "opencode.json";
    const std::string content =
        R"({"provider": {"other": {"options": {"apiKey": "secret"}}}})";
    const auto is_private = [&] {
        return fs::status(config_path).permissions() ==
               (fs::perms::owner_read | fs::perms::owner_write);
    };

    write_file(config_path, content);
    fs::permissions(config_path, fs::perms::owner_read | fs::perms::owner_write);
    lemon_cli::sync_agent_config_file(lemon_cli::opencode_profile(), "Lemonade",
                                      "http://localhost:13305/v1", "", {}, error);
    check("provider sync keeps a private opencode.json private", is_private());

    lemon_cli::sync_opencode_mcp_server("http://localhost:13305", false, error);
    check("MCP sync keeps a private opencode.json private", is_private());

    lemon_cli::remove_opencode_mcp_server(error);
    check("--no-mcp removal keeps a private opencode.json private", is_private());

    const fs::path target = config_dir / "dotfiles-opencode.json";
    fs::remove(config_path);
    write_file(target, content);
    fs::create_symlink(target, config_path);
    lemon_cli::sync_agent_config_file(lemon_cli::opencode_profile(), "Lemonade",
                                      "http://localhost:13305/v1", "", {}, error);
    lemon_cli::sync_opencode_mcp_server("http://127.0.0.1:9000", false, error);
    const json written = json::parse(read_file(target));
    check("sync writes through a symlinked opencode.json",
          fs::is_symlink(config_path) && written["provider"].contains("Lemonade") &&
              written["mcp"]["servers"]["lemonade"]["url"] == "http://127.0.0.1:9000/mcp");
    fs::remove(config_path);
}
#endif

}  // namespace

int main() {
    const fs::path config_home = fs::temp_directory_path() /
        ("lemonade-opencode-profile-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path config_dir = config_home / "opencode";
    fs::create_directories(config_dir);
#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME", config_home.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", config_home.string().c_str(), 1);
#endif

    test_version_parse();
    test_standalone_placement();
    test_inline_config();
    test_provider_block();
    test_api_key();
    test_mcp_servers(config_dir / "opencode.json");
    test_mcp_remove(config_dir / "opencode.json");
    test_codemode_config(config_dir / "opencode.json");
#ifndef _WIN32
    test_config_file_kept(config_dir);
#endif

    std::error_code ec;
    fs::remove_all(config_home, ec);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
