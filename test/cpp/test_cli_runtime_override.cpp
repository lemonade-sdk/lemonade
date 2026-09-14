#include <cstdio>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/runtime_config.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using lemon::CLIParser;
using lemon::ConfigFile;
using lemon::RuntimeConfig;

static int passed = 0;
static int failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("[PASS] %s\n", desc);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", desc);
        ++failures;
    }
}

static fs::path make_temp_cache_dir(const std::string& tag) {
    static int counter = 0;
    fs::path root = fs::temp_directory_path() / ("lemonade_cli_override_test_" + tag + "_" + std::to_string(counter++));
    fs::create_directories(root);
    return root;
}

int main() {
    {
        CLIParser parser;
        const char* argv[] = {"lemond", "--port", "9000", "--host", "0.0.0.0"};
        int res = parser.parse(5, const_cast<char**>(argv));
        check(res == 0 && parser.should_continue(), "CLIParser parses --port and --host successfully");
        auto cli_cfg = parser.get_config();
        check(cli_cfg.port == 9000, "CLIParser captured port 9000");
        check(cli_cfg.host == "0.0.0.0", "CLIParser captured host 0.0.0.0");
    }

    {
        fs::path temp_dir = make_temp_cache_dir("ephemeral");
        json initial_cfg = {
            {"port", 13305},
            {"host", "localhost"},
            {"log_level", "info"}
        };
        ConfigFile::save(temp_dir.string(), initial_cfg);

        CLIParser parser;
        const std::string temp_dir_arg = temp_dir.string();
        const char* argv[] = {"lemond", temp_dir_arg.c_str(), "--port", "8888", "--host", "127.0.0.1"};
        int res = parser.parse(6, const_cast<char**>(argv));
        check(res == 0 && parser.should_continue(), "CLIParser parses explicit directory with overrides successfully");
        auto cli_cfg = parser.get_config();
        check(cli_cfg.cache_dir == temp_dir_arg, "CLIParser preserves explicit cache directory");
        check(cli_cfg.config_dir == temp_dir_arg, "CLIParser uses cache directory as config directory for portable installs");

        json loaded = ConfigFile::load(cli_cfg.cache_dir, cli_cfg.config_dir);
        check(loaded["port"] == 13305, "Loaded config before override has original port");
        check(loaded["host"] == "localhost", "Loaded config before override has original host");

        RuntimeConfig runtime_cfg(loaded);
        if (cli_cfg.port != -1) {
            runtime_cfg.set_port_override(cli_cfg.port);
        }
        if (cli_cfg.host != "") {
            runtime_cfg.set_host_override(cli_cfg.host);
        }
        runtime_cfg.set_log_file_override("enabled");
        runtime_cfg.set_log_max_file_size_mb_override(50);
        runtime_cfg.set_log_max_files_override(10);

        check(runtime_cfg.port() == 8888, "RuntimeConfig::port() returns overridden port 8888");
        check(runtime_cfg.host() == "127.0.0.1", "RuntimeConfig::host() returns overridden host 127.0.0.1");
        check(runtime_cfg.log_file() == "enabled", "RuntimeConfig::log_file() returns overridden log_file");
        check(runtime_cfg.log_max_file_size_mb() == 50, "RuntimeConfig::log_max_file_size_mb() returns overridden max_file_size");
        check(runtime_cfg.log_max_files() == 10, "RuntimeConfig::log_max_files() returns overridden max_files");

        // Verify snapshot() does NOT leak the CLI overrides into the persistent representation
        json snap = runtime_cfg.snapshot();
        check(snap["port"] == 13305, "snapshot() contains persistent port 13305, not override");
        check(snap["host"] == "localhost", "snapshot() contains persistent host localhost, not override");
        check(!snap.contains("log_file") || snap["log_file"] == "auto", "snapshot() does not leak log_file override");

        // Simulate persisting a config update (e.g. log_level change) without dropping CLI overrides
        runtime_cfg.set(json{{"log_level", "debug"}});
        check(runtime_cfg.log_level() == "debug", "RuntimeConfig updated log_level");
        check(runtime_cfg.log_file() == "enabled", "RuntimeConfig::log_file() preserves CLI override after reconfiguration");
        check(runtime_cfg.log_max_file_size_mb() == 50, "RuntimeConfig::log_max_file_size_mb() preserves CLI override after reconfiguration");
        check(runtime_cfg.log_max_files() == 10, "RuntimeConfig::log_max_files() preserves CLI override after reconfiguration");

        ConfigFile::save(cli_cfg.config_dir, runtime_cfg.snapshot());

        // Re-read from disk to ensure config.json was updated with log_level but did NOT leak port/host/log overrides
        json disk_cfg = ConfigFile::load(cli_cfg.cache_dir, cli_cfg.config_dir);
        check(disk_cfg["log_level"] == "debug", "config.json on disk updated log_level to debug");
        check(disk_cfg["port"] == 13305, "config.json on disk preserved original port 13305 after config set");
        check(disk_cfg["host"] == "localhost", "config.json on disk preserved original host localhost after config set");

        // Verify removing overrides reverts to underlying config values
        runtime_cfg.set_port_override(std::nullopt);
        runtime_cfg.set_host_override(std::nullopt);
        runtime_cfg.set_log_file_override(std::nullopt);
        runtime_cfg.set_log_max_file_size_mb_override(std::nullopt);
        runtime_cfg.set_log_max_files_override(std::nullopt);
        check(runtime_cfg.port() == 13305, "RuntimeConfig::port() reverts to persistent port after clearing override");
        check(runtime_cfg.host() == "localhost", "RuntimeConfig::host() reverts to persistent host after clearing override");
        check(runtime_cfg.log_file() == "auto", "RuntimeConfig::log_file() reverts to default after clearing override");
        check(runtime_cfg.log_max_file_size_mb() == 10, "RuntimeConfig::log_max_file_size_mb() reverts to default after clearing override");
        check(runtime_cfg.log_max_files() == 5, "RuntimeConfig::log_max_files() reverts to default after clearing override");

        fs::remove_all(temp_dir);
    }

    {
        CLIParser parser;
        const char* argv[] = {"lemond"};
        int res = parser.parse(1, const_cast<char**>(argv));
        check(res == 0 && parser.should_continue(), "CLIParser parses default lemond invocation successfully");
        auto cli_cfg = parser.get_config();
        check(cli_cfg.port == -1, "Default port is -1 when omitted");
        check(cli_cfg.host.empty(), "Default host is empty when omitted");
    }

    std::printf("\nSummary: %d passed, %d failed\n", passed, failures);
    return failures == 0 ? 0 : 1;
}
