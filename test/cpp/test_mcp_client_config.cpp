#include "lemon/mcp_client.h"

#include <cassert>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    using lemon::McpClientManager;
    using lemon::json;

    const json raw = {
        {"id", "revit"},
        {"name", "Revit MCP"},
        {"transport", "stdio"},
        {"command", "python"},
        {"args", json::array({"-m", "revit_mcp_bridge"})},
        {"env", json{{"REVIT_PROFILE", "default"}}},
        {"working_dir", ""},
        {"enabled", true},
        {"timeout_ms", 5000},
    };

    auto cfg = McpClientManager::parse_server_config_json(raw);
    assert(cfg.id == "revit");
    assert(cfg.name == "Revit MCP");
    assert(cfg.transport == "stdio");
    assert(cfg.command == "python");
    assert(cfg.args.size() == 2);
    assert(cfg.env.at("REVIT_PROFILE") == "default");
    assert(cfg.timeout_ms == 5000);

    const json masked = McpClientManager::config_to_json(cfg, false);
    assert(masked.at("env").at("REVIT_PROFILE") == "***");
    const json unmasked = McpClientManager::config_to_json(cfg, true);
    assert(unmasked.at("env").at("REVIT_PROFILE") == "default");

    const std::string chat_name = McpClientManager::make_chat_tool_name("revit", "list elements!");
    assert(chat_name == "mcp_revit__list_elements");

    bool rejected = false;
    try {
        McpClientManager::parse_server_config_json(json{{"id", "bad id"}, {"command", "python"}});
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);

    auto generated = McpClientManager::parse_server_config_json(
        json{{"name", "My Server"}, {"command", "node"}, {"args", json::array({"server.js"})}},
        true);
    assert(generated.id == "my-server");

#if defined(LEMONADE_TEST_PYTHON) && defined(LEMONADE_TEST_MCP_STDIO_SERVER)
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const fs::path cache_dir = fs::temp_directory_path() /
        ("lemonade_mcp_client_test_" + std::to_string(unique));
    fs::create_directories(cache_dir);

    {
        McpClientManager manager(cache_dir.string());
        const json created = manager.upsert_server_json(json{
            {"name", "Mock MCP"},
            {"transport", "stdio"},
            {"command", std::string(LEMONADE_TEST_PYTHON)},
            {"args", json::array({std::string(LEMONADE_TEST_MCP_STDIO_SERVER)})},
            {"env", json{{"LEMONADE_MCP_TEST_SECRET", "super-secret-value"}}},
            {"timeout_ms", 5000},
        });

        const std::string id = created.at("server").at("id").get<std::string>();
        assert(!id.empty());

        const fs::path persisted_path = cache_dir / "mcp_servers.json";
        std::ifstream persisted_in(persisted_path);
        assert(persisted_in.good());
        const std::string persisted(
            (std::istreambuf_iterator<char>(persisted_in)),
            std::istreambuf_iterator<char>());
        assert(persisted.find("super-secret-value") == std::string::npos);
        assert(persisted.find("${LEMONADE_MCP_TEST_SECRET}") != std::string::npos);

        const json connected = manager.connect_server_json(id);
        assert(connected.at("server").value("connected", false));
        assert(connected.at("server").at("tools").is_array());
        assert(connected.at("server").at("tools").size() == 1);
        assert(connected.at("server").at("tools").at(0).at("name") == "echo");

        const json listed = manager.list_tools_json();
        assert(listed.at("tools").is_array());
        assert(listed.at("tools").size() == 1);
        assert(listed.at("tools").at(0).at("chat_name").get<std::string>().find("mcp_") == 0);

        const json call = manager.call_tool_json(id, json{
            {"name", "echo"},
            {"arguments", json{{"message", "hello from lemonade"}}},
            {"timeout_ms", 5000},
        });
        assert(call.at("result").at("content").at(0).at("text") == "hello from lemonade");

        manager.disconnect_server_json(id);
        manager.remove_server_json(id);
    }
    fs::remove_all(cache_dir);
#else
    std::cout << "mock stdio integration test skipped: Python fixture not configured\n";
#endif

    std::cout << "mcp client config tests passed\n";
    return 0;
}
