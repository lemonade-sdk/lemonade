#include "lemon/mcp_client.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void set_test_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name.c_str(), value.c_str()) != 0) {
        throw std::runtime_error("failed to set test environment variable");
    }
#else
    if (::setenv(name.c_str(), value.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set test environment variable");
    }
#endif
}

void unset_test_env(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    ::unsetenv(name.c_str());
#endif
}

bool throws(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void remove_test_directory(const fs::path& path) noexcept {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) {
        std::cerr << "warning: failed to remove MCP test directory '"
                  << path.string() << "': " << ec.message() << '\n';
    }
}

int run_tests() {
    using lemon::McpClientManager;
    using lemon::json;

    const json raw = {
        {"id", "revit"},
        {"name", "Revit MCP"},
        {"transport", "stdio"},
        {"command", "python"},
        {"args", json::array({"-m", "revit_mcp_bridge"})},
        {"env", json{{"REVIT_PROFILE", "${REVIT_PROFILE}"}}},
        {"working_dir", ""},
        {"enabled", true},
        {"timeout_ms", 5000},
    };

    const auto config = McpClientManager::parse_server_config_json(raw);
    assert(config.id == "revit");
    assert(config.name == "Revit MCP");
    assert(config.transport == "stdio");
    assert(config.command == "python");
    assert(config.args.size() == 2);
    assert(config.env.at("REVIT_PROFILE") == "${REVIT_PROFILE}");
    assert(config.timeout_ms == 5000);

    const json public_config = McpClientManager::config_to_json(config, false);
    assert(public_config.at("env").at("REVIT_PROFILE") == "${REVIT_PROFILE}");

    assert(throws([] {
        McpClientManager::parse_server_config_json(
            json{{"id", "raw-secret"},
                 {"command", "python"},
                 {"env", json{{"TOKEN", "plaintext-secret"}}}});
    }));
    assert(throws([] {
        McpClientManager::parse_server_config_json(
            json{{"id", "bad id"}, {"command", "python"}});
    }));
    assert(throws([] {
        McpClientManager::parse_server_config_json(
            json{{"id", "bad-timeout"},
                 {"command", "python"},
                 {"timeout_ms", 1}});
    }));

    const auto generated = McpClientManager::parse_server_config_json(
        json{{"name", "My Server"},
             {"command", "node"},
             {"args", json::array({"server.js"})}},
        true);
    assert(generated.id == "my-server");

    const std::string lossy_a =
        McpClientManager::make_chat_tool_name("server", "a b");
    const std::string lossy_b =
        McpClientManager::make_chat_tool_name("server", "a@b");
    assert(lossy_a != lossy_b);
    assert(lossy_a.size() <= 64);
    assert(lossy_b.size() <= 64);
    const std::string separator_a =
        McpClientManager::make_chat_tool_name("a", "b__c");
    const std::string separator_b =
        McpClientManager::make_chat_tool_name("a__b", "c");
    assert(separator_a != separator_b);

#if defined(LEMONADE_TEST_PYTHON) && defined(LEMONADE_TEST_MCP_STDIO_SERVER)
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path cache_dir = fs::temp_directory_path() /
                               ("lemonade_mcp_client_test_" +
                                std::to_string(unique));
    fs::create_directories(cache_dir);
    set_test_env("LEMONADE_MCP_TEST_SECRET", "super-secret-value");
    set_test_env("LEMONADE_MCP_INIT_DELAY", "0");

    try {
        McpClientManager manager(cache_dir.string());
        const json created = manager.upsert_server_json(
            json{{"name", "Mock MCP"},
                 {"transport", "stdio"},
                 {"command", std::string(LEMONADE_TEST_PYTHON)},
                 {"args",
                  json::array(
                      {std::string(LEMONADE_TEST_MCP_STDIO_SERVER)})},
                 {"env",
                  json{{"LEMONADE_MCP_TEST_SECRET",
                        "${LEMONADE_MCP_TEST_SECRET}"}}},
                 {"timeout_ms", 5000}});

        const std::string id =
            created.at("server").at("id").get<std::string>();
        assert(!id.empty());

        const fs::path persisted_path = cache_dir / "mcp_servers.json";
        std::string persisted;
        {
            std::ifstream persisted_input(persisted_path);
            if (!persisted_input) {
                throw std::runtime_error(
                    "failed to open persisted MCP configuration: " +
                    persisted_path.string());
            }
            persisted.assign(
                std::istreambuf_iterator<char>(persisted_input),
                std::istreambuf_iterator<char>());
        }
        assert(persisted.find("super-secret-value") == std::string::npos);
        assert(persisted.find("${LEMONADE_MCP_TEST_SECRET}") !=
               std::string::npos);

        const json connected = manager.connect_server_json(id);
        assert(connected.at("server").value("connected", false));
        assert(connected.at("server").at("protocol_version") ==
               "2025-11-25");
        assert(connected.at("server").at("tools").is_array());
        assert(connected.at("server").at("tools").size() == 3);

        const json call = manager.call_tool_json(
            id, json{{"name", "echo"},
                     {"arguments",
                      json{{"message", "hello from lemonade"}}},
                     {"timeout_ms", 5000}});
        assert(call.at("result").at("content").at(0).at("text") ==
               "hello from lemonade");
        assert(call.at("result")
                   .at("structuredContent")
                   .at("secret_seen") == true);

        // Multiple in-flight JSON-RPC requests must be correlated by ID without
        // serializing the full request/response wait under the runtime lock.
        std::vector<std::future<json>> parallel_calls;
        for (int i = 0; i < 16; ++i) {
            parallel_calls.push_back(std::async(std::launch::async, [&, i] {
                return manager.call_tool_json(
                    id, json{{"name", "echo"},
                             {"arguments",
                              json{{"message", "parallel-" +
                                                   std::to_string(i)}}},
                             {"timeout_ms", 5000}});
            }));
        }
        for (int i = 0; i < 16; ++i) {
            const json parallel =
                parallel_calls[static_cast<std::size_t>(i)].get();
            assert(parallel.at("result").at("content").at(0).at("text") ==
                   "parallel-" + std::to_string(i));
        }

        // A slow tool call must not block status snapshots or disconnect. The
        // original PR held Runtime::mutex while waiting for the response.
        auto slow_call = std::async(std::launch::async, [&] {
            return manager.call_tool_json(
                id, json{{"name", "sleep"},
                         {"arguments", json{{"seconds", 10.0}}},
                         {"timeout_ms", 30000}});
        });
        std::this_thread::sleep_for(250ms);

        const auto status_start = std::chrono::steady_clock::now();
        const json listed = manager.list_servers_json();
        const auto status_elapsed =
            std::chrono::steady_clock::now() - status_start;
        assert(status_elapsed < 1s);
        assert(listed.at("servers").is_array());

        const auto disconnect_start = std::chrono::steady_clock::now();
        manager.disconnect_server_json(id);
        const auto disconnect_elapsed =
            std::chrono::steady_clock::now() - disconnect_start;
        assert(disconnect_elapsed < 4s);
        assert(slow_call.wait_for(2s) == std::future_status::ready);
        assert(throws([&] { (void)slow_call.get(); }));

        // Reconnect and verify that a child process exiting without a response
        // wakes the request immediately rather than waiting for its full timeout.
        manager.connect_server_json(id);
        const auto exit_start = std::chrono::steady_clock::now();
        assert(throws([&] {
            manager.call_tool_json(
                id, json{{"name", "exit"},
                         {"arguments", json::object()},
                         {"timeout_ms", 10000}});
        }));
        const auto exit_elapsed =
            std::chrono::steady_clock::now() - exit_start;
        assert(exit_elapsed < 5s);

        manager.remove_server_json(id);

        // Disconnect must also cancel initialization in progress. This covers a
        // race where disconnect could previously return before connect installed
        // its child, allowing the connection to become live afterwards.
        set_test_env("LEMONADE_MCP_INIT_DELAY", "10");
        const json delayed = manager.upsert_server_json(
            json{{"name", "Delayed MCP"},
                 {"transport", "stdio"},
                 {"command", std::string(LEMONADE_TEST_PYTHON)},
                 {"args",
                  json::array(
                      {std::string(LEMONADE_TEST_MCP_STDIO_SERVER)})},
                 {"env",
                  json{{"LEMONADE_MCP_INIT_DELAY",
                        "${LEMONADE_MCP_INIT_DELAY}"}}},
                 {"timeout_ms", 30000}});
        const std::string delayed_id =
            delayed.at("server").at("id").get<std::string>();

        auto connecting = std::async(std::launch::async, [&] {
            return manager.connect_server_json(delayed_id);
        });
        std::this_thread::sleep_for(250ms);
        const auto cancel_connect_start = std::chrono::steady_clock::now();
        manager.disconnect_server_json(delayed_id);
        const auto cancel_connect_elapsed =
            std::chrono::steady_clock::now() - cancel_connect_start;
        assert(cancel_connect_elapsed < 4s);
        assert(connecting.wait_for(2s) == std::future_status::ready);
        assert(throws([&] { (void)connecting.get(); }));
        const json delayed_state = manager.list_servers_json();
        bool found_delayed_disconnected = false;
        for (const auto& server : delayed_state.at("servers")) {
            if (server.at("id") == delayed_id) {
                found_delayed_disconnected =
                    !server.value("connected", true);
            }
        }
        assert(found_delayed_disconnected);
        manager.remove_server_json(delayed_id);
    } catch (...) {
        unset_test_env("LEMONADE_MCP_TEST_SECRET");
        unset_test_env("LEMONADE_MCP_INIT_DELAY");
        remove_test_directory(cache_dir);
        throw;
    }

    unset_test_env("LEMONADE_MCP_TEST_SECRET");
    unset_test_env("LEMONADE_MCP_INIT_DELAY");
    remove_test_directory(cache_dir);
#else
    std::cout
        << "mock stdio integration test skipped: Python fixture not configured\n";
#endif

    std::cout << "mcp client config tests passed\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return run_tests();
    } catch (const std::exception& e) {
        std::cerr << "mcp_client_config failed: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "mcp_client_config failed with unknown exception\n";
        return 1;
    }
}