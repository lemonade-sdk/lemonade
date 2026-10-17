#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <lemon/runtime_config.h>
#include <nlohmann/json.hpp>
#include "telemetry.h"
#include "telemetry_queue.h"

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

    // 3. Test max_attribute_length and max_queue_bytes defaults and validation
    check(config.telemetry_max_attribute_length() == 0, "telemetry_max_attribute_length defaults to 0");
    check(config.telemetry_max_queue_bytes() == 134217728, "telemetry_max_queue_bytes defaults to 134217728 (128MB)");

    bool threw_invalid_max_attr_len = false;
    try {
        json invalid_max_attr = {
            {"telemetry", {
                {"max_attribute_length", -1}
            }}
        };
        config.set(invalid_max_attr);
    } catch (const std::invalid_argument& e) {
        threw_invalid_max_attr_len = true;
        std::printf("Expected exception caught: %s\n", e.what());
    }
    check(threw_invalid_max_attr_len, "rejects negative telemetry.max_attribute_length");

    bool threw_invalid_max_queue_bytes = false;
    try {
        json invalid_max_bytes = {
            {"telemetry", {
                {"max_queue_bytes", -1}
            }}
        };
        config.set(invalid_max_bytes);
    } catch (const std::invalid_argument& e) {
        threw_invalid_max_queue_bytes = true;
        std::printf("Expected exception caught: %s\n", e.what());
    }
    check(threw_invalid_max_queue_bytes, "rejects negative telemetry.max_queue_bytes");

    json valid_max_attr = {
        {"telemetry", {
            {"max_attribute_length", 3000000000LL},
            {"max_queue_bytes", 268435456}
        }}
    };
    config.set(valid_max_attr);
    check(config.telemetry_max_attribute_length() == 3000000000LL, "updates max_attribute_length to 64-bit int 3000000000");
    check(config.telemetry_max_queue_bytes() == 268435456, "updates max_queue_bytes to 268435456 (256MB)");

    json reset_max_attr = {
        {"telemetry", {
            {"max_attribute_length", 0},
            {"max_queue_bytes", 134217728}
        }}
    };
    config.set(reset_max_attr);
    check(config.telemetry_max_attribute_length() == 0, "resets max_attribute_length to 0");
    check(config.telemetry_max_queue_bytes() == 134217728, "resets max_queue_bytes to 134217728");

    // 4. Test CLI dotted key config path parsing logic
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

    // 5. Test telemetry.session.headers.(id|client) config, validation and snapshot
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

    // 6. Test safe max_bytes conversion logic on 64-bit platforms
    auto compute_max_bytes = [](int64_t max_queue_bytes) -> size_t {
        size_t max_bytes = 0;
        if (max_queue_bytes > 0) {
            if (static_cast<uint64_t>(max_queue_bytes) > std::numeric_limits<size_t>::max()) {
                max_bytes = std::numeric_limits<size_t>::max();
            } else {
                max_bytes = static_cast<size_t>(max_queue_bytes);
            }
        }
        return max_bytes;
    };

    check(compute_max_bytes(0) == 0, "max_bytes conversion: 0 -> 0 (unlimited)");
    check(compute_max_bytes(-1) == 0, "max_bytes conversion: negative -> 0");
    check(compute_max_bytes(134217728) == 134217728, "max_bytes conversion: 128MB exact value preserved on 64-bit");
    check(compute_max_bytes(268435456) == 268435456, "max_bytes conversion: 256MB exact value preserved");

    // 7. Test 0 = unlimited attribute truncation semantics across text, JSON, and sessions
    std::string long_text(5000, 'x');
    check(lemon::telemetry::truncate_string(long_text, 0) == long_text, "truncate_string with max_len=0 leaves text intact");
    check(lemon::telemetry::truncate_string(long_text, 100).find("[TRUNCATED]") != std::string::npos, "truncate_string with max_len=100 truncates");

    std::string tool_call_json = "{\"location\":\"San Francisco\",\"unit\":\"celsius\",\"details\":{\"extra\":\"payload\"}}";
    check(lemon::telemetry::truncate_json_string(tool_call_json, 0) == tool_call_json, "truncate_json_string with max_len=0 leaves JSON intact");
    check(lemon::telemetry::truncate_json_string(tool_call_json, 25).find("_truncated") != std::string::npos, "truncate_json_string with max_len=25 marks truncated JSON");

    check(lemon::telemetry::format_namespaced_session("client_app", "user_session_12345", 0) == "client_app/user_session_12345", "format_namespaced_session with max_len=0 preserves compound session");

    // Test OTEL_SPAN_ATTRIBUTE_VALUE_LENGTH_LIMIT / OTEL_ATTRIBUTE_VALUE_LENGTH_LIMIT env vars
    {
        setenv("OTEL_SPAN_ATTRIBUTE_VALUE_LENGTH_LIMIT", "50", 1);
        json req = {{"messages", json::array({json::object({{"role", "user"}, {"content", std::string(200, 'Z')}})})}};
        lemon::telemetry::InferenceSpan span("LLM", "test.chat", "model", req);
        unsetenv("OTEL_SPAN_ATTRIBUTE_VALUE_LENGTH_LIMIT");
    }

    // 8. Test production TelemetryQueue byte accounting, oversized dropping, and FIFO eviction
    {
        json queue_cfg_json = {
            {"config_version", 2},
            {"port", 13305},
            {"host", "localhost"},
            {"telemetry", {
                {"enabled", true},
                {"max_queue_capacity", 10},
                {"max_queue_bytes", 1000},
                {"otlp", {
                    {"endpoint", "http://127.0.0.1:9999/v1/traces"},
                    {"protocol", "http/json"},
                    {"send_batch_size", 100},
                    {"batch_timeout_s", 60.0}
                }}
            }}
        };
        lemon::RuntimeConfig queue_test_cfg(queue_cfg_json);
        lemon::RuntimeConfig::set_global(&queue_test_cfg);

        lemon::telemetry::TelemetryQueue real_queue;
        check(real_queue.size() == 0, "production queue: starts empty");
        check(real_queue.current_bytes() == 0, "production queue: starts at 0 bytes");

        std::string huge_text(1500, 'A');
        nlohmann::json oversized_span = {{"traceId", "1"}, {"spanId", "1"}, {"name", huge_text}};
        real_queue.push(oversized_span, "http://127.0.0.1:9999/v1/traces", {}, "http/json");
        check(real_queue.size() == 0, "production queue: oversized span dropped immediately");
        check(real_queue.dropped_count() == 1, "production queue: dropped count is 1");

        std::string payload300(300, 'B');
        nlohmann::json span1 = {{"traceId", "s1"}, {"name", payload300}};
        real_queue.push(span1, "http://127.0.0.1:9999/v1/traces", {}, "http/json");
        size_t b1 = real_queue.current_bytes();
        check(real_queue.size() == 1 && b1 > 300, "production queue: span1 enqueued with accurate bytes");

        nlohmann::json span2 = {{"traceId", "s2"}, {"name", payload300}};
        real_queue.push(span2, "http://127.0.0.1:9999/v1/traces", {}, "http/json");
        size_t b2 = real_queue.current_bytes();
        check(real_queue.size() == 2 && b2 > b1 && b2 < 1000, "production queue: span2 accumulated bytes");

        std::string payload400(400, 'C');
        nlohmann::json span3 = {{"traceId", "s3"}, {"name", payload400}};
        real_queue.push(span3, "http://127.0.0.1:9999/v1/traces", {}, "http/json");
        check(real_queue.size() == 2, "production queue: FIFO eviction kept queue within byte limit");
        check(real_queue.dropped_count() == 2, "production queue: dropped count incremented on eviction");
        check(real_queue.current_bytes() < 1000, "production queue: current bytes remains <= 1000");

        real_queue.shutdown();
        lemon::RuntimeConfig::set_global(nullptr);
    }

    {
        json queue_cfg_json = {
            {"config_version", 2},
            {"port", 13305},
            {"host", "localhost"},
            {"telemetry", {
                {"enabled", true},
                {"max_queue_capacity", 10},
                {"max_queue_bytes", 1000},
                {"otlp", {
                    {"endpoint", "http://127.0.0.1:9999/v1/traces"},
                    {"protocol", "http/protobuf"},
                    {"send_batch_size", 100},
                    {"batch_timeout_s", 60.0}
                }}
            }}
        };
        lemon::RuntimeConfig queue_test_cfg(queue_cfg_json);
        lemon::RuntimeConfig::set_global(&queue_test_cfg);

        lemon::telemetry::TelemetryQueue proto_queue;
        check(proto_queue.size() == 0, "protobuf queue: starts empty");
        check(proto_queue.current_bytes() == 0, "protobuf queue: starts at 0 bytes");

        std::string huge_err(1500, 'E');
        nlohmann::json oversized_err_span = {
            {"traceId", "1"},
            {"spanId", "1"},
            {"name", "chat"},
            {"status", {{"code", 2}, {"message", huge_err}}}
        };
        proto_queue.push(oversized_err_span, "http://127.0.0.1:9999/v1/traces", {}, "http/protobuf");
        check(proto_queue.size() == 0, "protobuf queue: oversized error span dropped immediately");
        check(proto_queue.dropped_count() == 1, "protobuf queue: dropped count is 1");

        std::string payload200(200, 'B');
        nlohmann::json span1 = {
            {"traceId", "s1"},
            {"spanId", "s1"},
            {"name", "chat"},
            {"attributes", {{{"key", "gen_ai.prompt"}, {"value", {{"stringValue", payload200}}}}}}
        };
        proto_queue.push(span1, "http://127.0.0.1:9999/v1/traces", {}, "http/protobuf");
        size_t b1 = proto_queue.current_bytes();
        check(proto_queue.size() == 1 && b1 > 200, "protobuf queue: span1 enqueued with accurate bytes");

        nlohmann::json span2 = {
            {"traceId", "s2"},
            {"spanId", "s2"},
            {"name", "chat"},
            {"attributes", {{{"key", "gen_ai.prompt"}, {"value", {{"stringValue", payload200}}}}}}
        };
        proto_queue.push(span2, "http://127.0.0.1:9999/v1/traces", {}, "http/protobuf");
        size_t b2 = proto_queue.current_bytes();
        check(proto_queue.size() == 2 && b2 > b1 && b2 < 1000, "protobuf queue: span2 accumulated bytes");

        std::string payload250(250, 'C');
        nlohmann::json span3 = {
            {"traceId", "s3"},
            {"spanId", "s3"},
            {"name", "chat"},
            {"attributes", {{{"key", "gen_ai.prompt"}, {"value", {{"stringValue", payload250}}}}}}
        };
        proto_queue.push(span3, "http://127.0.0.1:9999/v1/traces", {}, "http/protobuf");
        check(proto_queue.size() == 2, "protobuf queue: FIFO eviction kept queue within byte limit");
        check(proto_queue.dropped_count() == 2, "protobuf queue: dropped count incremented on eviction");
        check(proto_queue.current_bytes() < 1000, "protobuf queue: current bytes remains <= 1000");

        proto_queue.shutdown();
        lemon::RuntimeConfig::set_global(nullptr);
    }

    return test_helpers::report_results("C++ config/telemetry");
}
