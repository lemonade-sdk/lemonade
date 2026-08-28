#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <lemon/cli_parser.h>
#include <lemon/config_file.h>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using lemon::ConfigFile;
using lemon::RuntimeConfig;
using test_helpers::check;

int main() {
    std::puts("=== RUNNING RUNTIME CONFIG DISCOVERY TESTS ===");

    json base_cfg = {
        {"config_version", 2},
        {"port", 13305},
        {"host", "localhost"},
        {"broadcast", true}
    };

    RuntimeConfig config(base_cfg);

    // 1. Test broadcast initial value and updates
    check(config.broadcast() == true, "initial broadcast is true");
    config.set({{"broadcast", false}});
    check(config.broadcast() == false, "broadcast updated to false");
    config.set({{"broadcast", true}});
    check(config.broadcast() == true, "broadcast updated to true");

    // 2. Test legacy no_broadcast alias mapping via set()
    config.set({{"no_broadcast", true}});
    check(config.broadcast() == false, "legacy no_broadcast: true sets broadcast to false");
    check(config.snapshot()["broadcast"] == false, "snapshot contains broadcast: false");
    check(!config.snapshot().contains("no_broadcast"), "snapshot does not contain legacy no_broadcast");

    config.set({{"no_broadcast", false}});
    check(config.broadcast() == true, "legacy no_broadcast: false sets broadcast to true");
    check(config.snapshot()["broadcast"] == true, "snapshot contains broadcast: true");
    check(!config.snapshot().contains("no_broadcast"), "snapshot does not contain legacy no_broadcast");

    // 3. Validation: rejects non-boolean values and conflicting keys
    bool threw_invalid_broadcast = false;
    try {
        config.set({{"broadcast", "not-a-bool"}});
    } catch (const std::invalid_argument& e) {
        threw_invalid_broadcast = true;
    }
    check(threw_invalid_broadcast, "rejects non-boolean broadcast");

    bool threw_invalid_no_broadcast = false;
    try {
        config.set({{"no_broadcast", 123}});
    } catch (const std::invalid_argument& e) {
        threw_invalid_no_broadcast = true;
    }
    check(threw_invalid_no_broadcast, "rejects non-boolean no_broadcast");

    bool threw_conflicting = false;
    try {
        config.set({{"broadcast", true}, {"no_broadcast", false}});
    } catch (const std::invalid_argument& e) {
        threw_conflicting = true;
    }
    check(threw_conflicting, "rejects conflicting broadcast and no_broadcast in set()");

    // 4. Test transient override does NOT leak into snapshot()
    config.set({{"broadcast", true}});
    config.set_broadcast_override(false);
    check(config.broadcast() == false, "broadcast_override(false) takes effect at runtime");
    check(config.snapshot()["broadcast"] == true, "broadcast_override does NOT mutate snapshot()");

    // 5. Override -> config set transition and side-effect callback firing
    // Case A: Persisted broadcast=false, startup override=true, user sets broadcast=false
    config.set({{"broadcast", false}});
    config.set_broadcast_override(true);
    check(config.broadcast() == true, "effective broadcast is true due to override");

    bool side_effect_fired_a = false;
    json side_effect_diff_a = json::object();
    config.set({{"broadcast", false}}, [&](const json& diff) {
        side_effect_fired_a = true;
        side_effect_diff_a = diff;
    });
    check(config.broadcast() == false, "broadcast becomes false after set()");
    check(side_effect_fired_a, "side-effect callback fires when effective broadcast changes (override cleared)");
    check(side_effect_diff_a.contains("broadcast") && side_effect_diff_a["broadcast"] == false,
          "applied_diff reflects broadcast change from override true -> false");

    // Case B: Persisted broadcast=true, startup override=false, user sets broadcast=true
    config.set({{"broadcast", true}});
    config.set_broadcast_override(false);
    check(config.broadcast() == false, "effective broadcast is false due to override");

    bool side_effect_fired_b = false;
    json side_effect_diff_b = json::object();
    config.set({{"broadcast", true}}, [&](const json& diff) {
        side_effect_fired_b = true;
        side_effect_diff_b = diff;
    });
    check(config.broadcast() == true, "broadcast becomes true after set()");
    check(side_effect_fired_b, "side-effect callback fires when effective broadcast changes (override cleared)");
    check(side_effect_diff_b.contains("broadcast") && side_effect_diff_b["broadcast"] == true,
          "applied_diff reflects broadcast change from override false -> true");

    // 6. Test Server CLI parser flags
    lemon::CLIParser server_parser_1;
    const char* argv1[] = {"lemond", "--no-broadcast"};
    check(server_parser_1.parse(2, const_cast<char**>(argv1)) == 0, "CLIParser parses --no-broadcast");
    check(server_parser_1.get_config().broadcast.has_value() && *server_parser_1.get_config().broadcast == false,
          "--no-broadcast sets broadcast to false");

    lemon::CLIParser server_parser_2;
    const char* argv2[] = {"lemond", "--broadcast"};
    check(server_parser_2.parse(2, const_cast<char**>(argv2)) == 0, "CLIParser parses --broadcast");
    check(server_parser_2.get_config().broadcast.has_value() && *server_parser_2.get_config().broadcast == true,
          "--broadcast sets broadcast to true");

    lemon::CLIParser server_parser_3;
    const char* argv3[] = {"lemond"};
    check(server_parser_3.parse(1, const_cast<char**>(argv3)) == 0, "CLIParser parses default invocation");
    check(!server_parser_3.get_config().broadcast.has_value(),
          "default invocation leaves broadcast unset (nullopt)");

    // 7. Test legacy constructor JSON migration
    json legacy_cfg_true = {
        {"config_version", 2},
        {"port", 13305},
        {"host", "localhost"},
        {"no_broadcast", true}
    };
    RuntimeConfig config_legacy_true(legacy_cfg_true);
    check(config_legacy_true.broadcast() == false, "legacy no_broadcast: true migrates to broadcast: false in ctor");
    check(config_legacy_true.snapshot()["broadcast"] == false, "snapshot has broadcast: false");
    check(!config_legacy_true.snapshot().contains("no_broadcast"), "legacy no_broadcast removed from snapshot");

    json legacy_cfg_false = {
        {"config_version", 2},
        {"port", 13305},
        {"host", "localhost"},
        {"no_broadcast", false}
    };
    RuntimeConfig config_legacy_false(legacy_cfg_false);
    check(config_legacy_false.broadcast() == true, "legacy no_broadcast: false migrates to broadcast: true in ctor");
    check(config_legacy_false.snapshot()["broadcast"] == true, "snapshot has broadcast: true");
    check(!config_legacy_false.snapshot().contains("no_broadcast"), "legacy no_broadcast removed from snapshot");

    // 8. Test real ConfigFile::load() and save() migration on disk
    fs::path temp_test_dir = fs::temp_directory_path() / ("lemonade_test_discovery_" + std::to_string(std::time(nullptr)));
    fs::create_directories(temp_test_dir);

    try {
        // Write initial config with legacy no_broadcast: true
        fs::path cfg_file = temp_test_dir / "config.json";
        {
            std::ofstream out(cfg_file);
            out << "{\"config_version\": 2, \"port\": 13305, \"host\": \"localhost\", \"no_broadcast\": true}\n";
        }

        json loaded = ConfigFile::load(temp_test_dir.string(), temp_test_dir.string());
        check(loaded["broadcast"] == false, "ConfigFile::load migrates legacy no_broadcast: true to broadcast: false");
        check(!loaded.contains("no_broadcast"), "ConfigFile::load removes legacy no_broadcast from in-memory object");

        // Verify disk file was saved with migrated keys
        std::ifstream saved_in(cfg_file);
        json saved_disk = json::parse(saved_in);
        check(saved_disk["broadcast"] == false, "saved disk config.json contains broadcast: false");
        check(!saved_disk.contains("no_broadcast"), "saved disk config.json does not contain no_broadcast");
    } catch (const std::exception& e) {
        check(false, (std::string("ConfigFile::load disk migration failed: ") + e.what()).c_str());
    }
    fs::remove_all(temp_test_dir);

    // 9. Test deployment defaults (LEMONADE_DEFAULTS_PATH) with legacy no_broadcast: true
    fs::path temp_defaults_dir = fs::temp_directory_path() / ("lemonade_test_defaults_" + std::to_string(std::time(nullptr)));
    fs::create_directories(temp_defaults_dir);

    try {
        fs::path defaults_file = temp_defaults_dir / "custom_defaults.json";
        {
            std::ofstream out(defaults_file);
            out << "{\"no_broadcast\": true}\n";
        }

#ifdef _WIN32
        _putenv_s("LEMONADE_DEFAULTS_PATH", defaults_file.string().c_str());
#else
        setenv("LEMONADE_DEFAULTS_PATH", defaults_file.string().c_str(), 1);
#endif

        json defaults = ConfigFile::get_defaults();
        check(defaults["broadcast"] == false, "LEMONADE_DEFAULTS_PATH legacy no_broadcast: true overrides broadcast to false");
        check(!defaults.contains("no_broadcast"), "LEMONADE_DEFAULTS_PATH removes legacy no_broadcast from defaults object");

        // When user creates a fresh config under these defaults, broadcast is false
        fs::path fresh_cache_dir = temp_defaults_dir / "cache";
        json fresh_loaded = ConfigFile::load(fresh_cache_dir.string(), fresh_cache_dir.string());
        check(fresh_loaded["broadcast"] == false, "fresh config inherited broadcast=false from deployment defaults");

#ifdef _WIN32
        _putenv_s("LEMONADE_DEFAULTS_PATH", "");
#else
        unsetenv("LEMONADE_DEFAULTS_PATH");
#endif
    } catch (const std::exception& e) {
        check(false, (std::string("LEMONADE_DEFAULTS_PATH test failed: ") + e.what()).c_str());
    }
    fs::remove_all(temp_defaults_dir);

    // 10. Test that malformed legacy values in config.json are rejected
    fs::path malformed_dir = fs::temp_directory_path() / ("lemonade_test_malformed_" + std::to_string(std::time(nullptr)));
    fs::create_directories(malformed_dir);

    try {
        fs::path cfg_file = malformed_dir / "config.json";
        {
            std::ofstream out(cfg_file);
            out << "{\"config_version\": 2, \"port\": 13305, \"no_broadcast\": \"true\"}\n";
        }

        bool threw_malformed = false;
        try {
            ConfigFile::load(malformed_dir.string(), malformed_dir.string());
        } catch (const std::invalid_argument&) {
            threw_malformed = true;
        }
        check(threw_malformed, "ConfigFile::load rejects malformed non-boolean no_broadcast string");
    } catch (const std::exception& e) {
        check(false, (std::string("malformed test failed: ") + e.what()).c_str());
    }
    fs::remove_all(malformed_dir);

    return test_helpers::report_results("C++ config/discovery");
}
