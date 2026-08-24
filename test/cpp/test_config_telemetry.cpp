#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::RuntimeConfig;
using test_helpers::check;
using test_helpers::parse_cli_args;

int main() {
    std::puts("=== RUNNING RUNTIME CONFIG & TELEMETRY TESTS ===");

    json base_cfg = {
        {"config_version", 2},
        {"port", 13305},
        {"host", "localhost"},
        {"telemetry", {
            {"enabled", false},
            {"hide_inputs", false},
            {"hide_outputs", false},
            {"hide_thinking", false},
            {"max_queue_capacity", 1000},
            {"otlp", {
                {"endpoint", "http://localhost:4318/v1/traces"},
                {"protocol", "http/protobuf"},
                {"semantics", {"openinference", "otel_genai"}},
                {"headers", json::object()},
                {"max_retries", 0},
                {"retry_backoff_base_s", 5.0},
                {"send_batch_size", 100},
                {"batch_timeout_s", 1.0}
            }}
        }}
    };

    RuntimeConfig config(base_cfg);

    // 1. Test recursive merge: toggling telemetry should preserve existing otlp.* settings
    json toggle_off = {
        {"telemetry", {
            {"enabled", false}
        }}
    };
    config.set(toggle_off);
    json snapshot = config.snapshot();
    check(snapshot["telemetry"]["enabled"] == false, "telemetry toggled off");
    check(snapshot["telemetry"]["otlp"]["endpoint"] == "http://localhost:4318/v1/traces", "otlp.endpoint preserved on toggle off");
    check(snapshot["telemetry"]["otlp"]["semantics"] == json::array({"openinference", "otel_genai"}), "otlp.semantics preserved on toggle off");

    json toggle_on = {
        {"telemetry", {
            {"enabled", true}
        }}
    };
    config.set(toggle_on);
    snapshot = config.snapshot();
    check(snapshot["telemetry"]["enabled"] == true, "telemetry toggled on");
    check(snapshot["telemetry"]["otlp"]["endpoint"] == "http://localhost:4318/v1/traces", "otlp.endpoint preserved on toggle on");
    check(snapshot["telemetry"]["otlp"]["semantics"] == json::array({"openinference", "otel_genai"}), "otlp.semantics preserved on toggle on");

    // 2. Test validation: rejecting unknown telemetry / OTLP subkeys
    bool threw_unknown_telemetry = false;
    try {
        json invalid_telemetry = {
            {"telemetry", {
                {"unknown_field", true}
            }}
        };
        config.set(invalid_telemetry);
    } catch (const std::invalid_argument& e) {
        threw_unknown_telemetry = true;
        std::printf("Expected exception caught: %s\n", e.what());
    }
    check(threw_unknown_telemetry, "rejects unknown telemetry subkey");

    bool threw_unknown_otlp = false;
    try {
        json invalid_otlp = {
            {"telemetry", {
                {"otlp", {
                    {"unknown_otlp_field", "val"}
                }}
            }}
        };
        config.set(invalid_otlp);
    } catch (const std::invalid_argument& e) {
        threw_unknown_otlp = true;
        std::printf("Expected exception caught: %s\n", e.what());
    }
    check(threw_unknown_otlp, "rejects unknown telemetry.otlp subkey");

    // 3. Test CLI dotted key config path parsing logic
    std::vector<std::string> cli_args = {
        "telemetry.otlp.endpoint=http://127.0.0.1:5555/v1/traces",
        "telemetry.otlp.protocol=http/json",
        "telemetry.otlp.semantics=[\"openinference\"]",
        "telemetry.otlp.headers={\"Authorization\":\"Bearer test-key\"}",
        "port=9090"
    };
    json cli_updates = parse_cli_args(cli_args);
    check(cli_updates["port"] == 9090, "CLI parses top-level key");
    check(cli_updates["telemetry"]["otlp"]["endpoint"] == "http://127.0.0.1:5555/v1/traces", "CLI parses 3-level dotted path endpoint");
    check(cli_updates["telemetry"]["otlp"]["protocol"] == "http/json", "CLI parses 3-level dotted path protocol");
    check(cli_updates["telemetry"]["otlp"]["semantics"].is_array(), "CLI parses JSON array for semantics");
    check(cli_updates["telemetry"]["otlp"]["semantics"][0] == "openinference", "CLI parsed array value matches");
    check(cli_updates["telemetry"]["otlp"]["headers"].is_object(), "CLI parses JSON object for headers");
    check(cli_updates["telemetry"]["otlp"]["headers"]["Authorization"] == "Bearer test-key", "CLI parsed object field matches");

    // 4. Test telemetry.session.headers.(id|client) config, validation and snapshot
    json session_cfg = {
        {"telemetry", {
            {"session", {
                {"headers", {
                    {"id", json::array({"x-custom-session", "x-alt-session"})},
                    {"client", json::array({"x-custom-client"})}
                }}
            }}
        }}
    };
    config.set(session_cfg);
    snapshot = config.snapshot();
    check(snapshot["telemetry"]["session"]["headers"]["id"].is_array(), "telemetry.session.headers.id is array in snapshot");
    check(snapshot["telemetry"]["session"]["headers"]["id"].size() == 2, "telemetry.session.headers.id has 2 entries");
    check(config.telemetry_session_headers_id().size() == 2, "telemetry_session_headers_id returns 2 headers");
    check(config.telemetry_session_headers_id()[0] == "x-custom-session", "telemetry_session_headers_id first header matches");
    check(config.telemetry_session_headers_client().size() == 1, "telemetry_session_headers_client returns 1 header");
    check(config.telemetry_session_headers_client()[0] == "x-custom-client", "telemetry_session_headers_client header matches");

    // Test rejection of non-string elements in array
    bool threw_invalid_element = false;
    try {
        json invalid_sess = {
            {"telemetry", {
                {"session", {
                    {"headers", {
                        {"id", json::array({123})}
                    }}
                }}
            }}
        };
        config.set(invalid_sess);
    } catch (const std::invalid_argument& e) {
        threw_invalid_element = true;
        std::printf("Expected exception caught: %s\n", e.what());
    }
    check(threw_invalid_element, "rejects non-string elements in telemetry.session.headers.id");

    // Test single string format for id and client
    json single_str_sess = {
        {"telemetry", {
            {"session", {
                {"headers", {
                    {"id", "x-single-session"},
                    {"client", "x-single-client"}
                }}
            }}
        }}
    };
    config.set(single_str_sess);
    check(config.telemetry_session_headers_id().size() == 1, "telemetry_session_headers_id accepts single string header");
    check(config.telemetry_session_headers_id()[0] == "x-single-session", "single header matches");
    check(config.telemetry_session_headers_client().size() == 1, "telemetry_session_headers_client accepts single string client header");
    check(config.telemetry_session_headers_client()[0] == "x-single-client", "single client header matches");

    // Test CLI parsing with dotted syntax
    std::vector<std::string> session_cli_args = {
        "telemetry.session.headers.id=[\"x-cli-session\"]",
        "telemetry.session.headers.client=x-cli-client"
    };
    json session_cli_updates = parse_cli_args(session_cli_args);
    config.set(session_cli_updates);
    check(config.telemetry_session_headers_id().size() == 1, "CLI sets telemetry.session.headers.id");
    check(config.telemetry_session_headers_id()[0] == "x-cli-session", "CLI session header matches");
    check(config.telemetry_session_headers_client().size() == 1, "CLI sets telemetry.session.headers.client");
    check(config.telemetry_session_headers_client()[0] == "x-cli-client", "CLI client header matches");

    return test_helpers::report_results("C++ config/telemetry");
}
