#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lemon/runtime_config.h>
#include <lemon/utils/json_utils.h>
#include <nlohmann/json.hpp>

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
    // 5. Test telemetry.otlp.headers validation and replacement
    auto check_header_throws = [&config](const json& headers_obj, const std::string& desc) {
        bool threw = false;
        try {
            json payload = {
                {"telemetry", {
                    {"otlp", {
                        {"headers", headers_obj}
                    }}
                }}
            };
            config.set(payload);
        } catch (const std::invalid_argument& e) {
            threw = true;
            std::printf("Expected exception caught (%s): %s\n", desc.c_str(), e.what());
        }
        check(threw, desc.c_str());
    };

    check_header_throws(json::array(), "rejects non-object headers");
    check_header_throws({{"X-Test", 123}}, "rejects non-string header value");
    check_header_throws({{"", "val"}}, "rejects empty header key");
    check_header_throws({{"   ", "val"}}, "rejects whitespace-only header key");
    check_header_throws({{"X-Bad\nKey", "val"}}, "rejects header key with LF");
    check_header_throws({{"X-Bad\rKey", "val"}}, "rejects header key with CR");
    check_header_throws({{"\nLeadingLF", "val"}}, "rejects header key with leading LF");
    check_header_throws({{"TrailingCRLF\r\n", "val"}}, "rejects header key with trailing CRLF");
    check_header_throws({{"Key", "val\r\n"}}, "rejects header value with trailing CRLF");
    check_header_throws({{"Key", "val\nval"}}, "rejects header value with embedded LF");
    check_header_throws({{"X-BadKey", std::string("val\0bad", 7)}}, "rejects header value with NUL");
    check_header_throws({{std::string("bad\0key", 7), "val"}}, "rejects header key with NUL");
    check_header_throws({{"Content-Type", "application/json"}}, "rejects overriding Content-Type");
    check_header_throws({{"content-type", "application/json"}}, "rejects overriding content-type lowercase");
    check_header_throws({{"Content-Length", "100"}}, "rejects overriding Content-Length");
    check_header_throws({{"content-length", "100"}}, "rejects overriding content-length lowercase");
    check_header_throws({{"Content-Type: foo", "application/json"}}, "rejects key with colon");
    check_header_throws({{"X Header", "val"}}, "rejects key with space");
    check_header_throws({{"X[Header]", "val"}}, "rejects key with delimiter");

    // Test valid headers update and replacement
    json valid_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"X-Custom-Header", "CustomVal"},
                    {"Authorization", "Bearer token123"}
                }}
            }}
        }}
    };
    config.set(valid_headers);
    check(config.telemetry_otlp_headers().size() == 2, "telemetry_otlp_headers returns 2 headers");
    check(config.telemetry_otlp_headers()["X-Custom-Header"] == "CustomVal", "custom header matches");
    check(config.telemetry_otlp_headers()["Authorization"] == "Bearer token123", "auth header matches");

    // Test replacing headers with new headers replaces rather than accumulates
    json replaced_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"X-Another", "NewVal"}
                }}
            }}
        }}
    };
    config.set(replaced_headers);
    check(config.telemetry_otlp_headers().size() == 1, "replacing headers reduces count to 1");
    check(config.telemetry_otlp_headers().count("X-Custom-Header") == 0, "previous custom header is gone");
    check(config.telemetry_otlp_headers()["X-Another"] == "NewVal", "new header matches");

    // Test clearing headers with empty object
    json empty_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", json::object()}
            }}
        }}
    };
    config.set(empty_headers);
    check(config.telemetry_otlp_headers().empty(), "clearing headers with empty object results in empty map");

    // Test whitespace normalization in config.set
    json spaced_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"  X-Spaced-Key  ", "  Spaced-Val  "}
                }}
            }}
        }}
    };
    config.set(spaced_headers);
    check(config.telemetry_otlp_headers().size() == 1, "whitespace headers accepted and trimmed");
    check(config.telemetry_otlp_headers().count("X-Spaced-Key") == 1, "trimmed key exists");
    check(config.telemetry_otlp_headers()["X-Spaced-Key"] == "Spaced-Val", "trimmed val matches");
    check(config.snapshot()["telemetry"]["otlp"]["headers"].contains("X-Spaced-Key"), "config snapshot stores trimmed key");

    // Test startup sanitization: bad headers from existing config.json are filtered out on construction
    json bad_startup_config = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"Content-Type", "application/json"},
                    {"Content-Length", "100"},
                    {"X-Bad\nKey", "val"},
                    {"", "empty_key"},
                    {"   ", "whitespace_key"},
                    {"  X-Startup-Valid  ", "  Startup-Val  "}
                }}
            }}
        }}
    };
    RuntimeConfig startup_cfg(bad_startup_config);
    check(startup_cfg.telemetry_otlp_headers().size() == 1, "startup sanitization filters invalid and keeps valid");
    check(startup_cfg.telemetry_otlp_headers().count("Content-Type") == 0, "startup filters Content-Type");
    check(startup_cfg.telemetry_otlp_headers().count("Content-Length") == 0, "startup filters Content-Length");
    check(startup_cfg.telemetry_otlp_headers().count("X-Bad\nKey") == 0, "startup filters key with newline");
    check(startup_cfg.telemetry_otlp_headers().count("X-Startup-Valid") == 1, "startup normalizes valid key");
    check(startup_cfg.telemetry_otlp_headers()["X-Startup-Valid"] == "Startup-Val", "startup normalizes valid val");

    // Test ConfigFile::load replace and clear semantics with default/distro headers
    json distro_defaults = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"X-Distro-Header", "DistroVal"}
                }}
            }}
        }}
    };
    json user_loaded_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", {
                    {"X-User-Header", "UserVal"}
                }}
            }}
        }}
    };
    json merged_user = lemon::utils::JsonUtils::merge(distro_defaults, user_loaded_headers);
    if (user_loaded_headers.contains("telemetry") && user_loaded_headers["telemetry"].is_object() &&
        user_loaded_headers["telemetry"].contains("otlp") && user_loaded_headers["telemetry"]["otlp"].is_object() &&
        user_loaded_headers["telemetry"]["otlp"].contains("headers")) {
        merged_user["telemetry"]["otlp"]["headers"] = user_loaded_headers["telemetry"]["otlp"]["headers"];
    }
    check(merged_user["telemetry"]["otlp"]["headers"].size() == 1, "user headers replace distro defaults");
    check(merged_user["telemetry"]["otlp"]["headers"].contains("X-User-Header"), "user header present");
    check(!merged_user["telemetry"]["otlp"]["headers"].contains("X-Distro-Header"), "distro header replaced");

    json user_clear_headers = {
        {"telemetry", {
            {"otlp", {
                {"headers", json::object()}
            }}
        }}
    };
    json merged_clear = lemon::utils::JsonUtils::merge(distro_defaults, user_clear_headers);
    if (user_clear_headers.contains("telemetry") && user_clear_headers["telemetry"].is_object() &&
        user_clear_headers["telemetry"].contains("otlp") && user_clear_headers["telemetry"]["otlp"].is_object() &&
        user_clear_headers["telemetry"]["otlp"].contains("headers")) {
        merged_clear["telemetry"]["otlp"]["headers"] = user_clear_headers["telemetry"]["otlp"]["headers"];
    }
    check(merged_clear["telemetry"]["otlp"]["headers"].empty(), "user empty headers clear distro defaults");

    // Test that explicit empty headers override is preserved across prune_matching
    json user_cfg_to_prune = {
        {"config_version", 2},
        {"telemetry", {
            {"otlp", {
                {"headers", json::object()}
            }}
        }}
    };
    bool explicit_h = (user_cfg_to_prune.contains("telemetry") && user_cfg_to_prune["telemetry"].contains("otlp") &&
                       user_cfg_to_prune["telemetry"]["otlp"].contains("headers"));
    json h_override = user_cfg_to_prune["telemetry"]["otlp"]["headers"];
    lemon::utils::JsonUtils::prune_matching(user_cfg_to_prune, distro_defaults);
    if (explicit_h) {
        if (!user_cfg_to_prune.contains("config_version")) {
            user_cfg_to_prune["config_version"] = distro_defaults.value("config_version", 2);
        }
        user_cfg_to_prune["telemetry"]["otlp"]["headers"] = h_override;
    }
    check(user_cfg_to_prune.contains("telemetry"), "user_cfg retains telemetry after prune");
    check(user_cfg_to_prune["telemetry"]["otlp"]["headers"].empty(), "user_cfg retains empty headers after prune");
    json reloaded_pruned = lemon::utils::JsonUtils::merge(distro_defaults, user_cfg_to_prune);
    if (user_cfg_to_prune.contains("telemetry") && user_cfg_to_prune["telemetry"].contains("otlp") &&
        user_cfg_to_prune["telemetry"]["otlp"].contains("headers")) {
        reloaded_pruned["telemetry"]["otlp"]["headers"] = user_cfg_to_prune["telemetry"]["otlp"]["headers"];
    }
    check(reloaded_pruned["telemetry"]["otlp"]["headers"].empty(), "reloaded config preserves cleared distro defaults");

    return test_helpers::report_results("C++ config/telemetry");
}
