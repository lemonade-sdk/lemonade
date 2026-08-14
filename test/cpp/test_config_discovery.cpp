#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <lemon/cli_parser.h>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::RuntimeConfig;
using test_helpers::check;
using test_helpers::parse_cli_args;

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

    // 3. Validation: rejects non-boolean values
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

    // 4. Test transient override does NOT leak into snapshot()
    config.set({{"broadcast", true}});
    config.set_broadcast_override(false);
    check(config.broadcast() == false, "broadcast_override(false) takes effect at runtime");
    check(config.snapshot()["broadcast"] == true, "broadcast_override does NOT mutate snapshot()");

    // 5. Explicit set() clears transient override
    config.set({{"broadcast", true}});
    check(config.broadcast() == true, "explicit set() clears override and applies new value");

    // 6. Test CLI parsing of broadcast and no-broadcast
    std::vector<std::string> cli_args = {
        "broadcast=false",
        "no-broadcast=true"
    };
    json cli_updates = parse_cli_args(cli_args);
    check(cli_updates["broadcast"] == false, "CLI parses broadcast=false");
    check(cli_updates["no_broadcast"] == true, "CLI parses and normalizes no-broadcast to no_broadcast");

    // 7. Test Server CLI parser flags
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

    // 8. Test legacy constructor JSON migration
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

    return test_helpers::report_results("C++ config/discovery");
}
