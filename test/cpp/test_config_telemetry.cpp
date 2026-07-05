#include <cassert>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <lemon/runtime_config.h>

using json = nlohmann::json;
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

// Replicate CLI parser logic to test it unit-wise
static json parse_cli_args(const std::vector<std::string>& args) {
    auto normalize_key = [](std::string s) {
        std::replace(s.begin(), s.end(), '-', '_');
        return s;
    };
    auto parse_typed_value = [](const std::string& val) -> json {
        if (val == "true") return true;
        if (val == "false") return false;
        try {
            size_t idx;
            int i = std::stoi(val, &idx);
            if (idx == val.size()) return i;
        } catch (...) {}
        try {
            size_t idx;
            double d = std::stod(val, &idx);
            if (idx == val.size()) return d;
        } catch (...) {}
        return val;
    };

    json updates = json::object();
    for (const auto& arg : args) {
        size_t eq_pos = arg.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) continue;
        std::string key = arg.substr(0, eq_pos);
        std::string value = arg.substr(eq_pos + 1);

        std::vector<std::string> path;
        size_t last_pos = 0;
        while (true) {
            size_t next_dot = key.find('.', last_pos);
            if (next_dot == std::string::npos) {
                std::string part = key.substr(last_pos);
                if (!part.empty()) {
                    path.push_back(normalize_key(part));
                }
                break;
            }
            std::string part = key.substr(last_pos, next_dot - last_pos);
            if (!part.empty()) {
                path.push_back(normalize_key(part));
            }
            last_pos = next_dot + 1;
        }

        if (path.empty()) continue;

        json* current = &updates;
        for (size_t i = 0; i < path.size(); ++i) {
            const std::string& k = path[i];
            if (i == path.size() - 1) {
                (*current)[k] = parse_typed_value(value);
            } else {
                if (!current->contains(k) || !(*current)[k].is_object()) {
                    (*current)[k] = json::object();
                }
                current = &((*current)[k]);
            }
        }
    }
    return updates;
}

int main() {
    std::puts("=== RUNNING RUNTIME CONFIG & TELEMETRY TESTS ===");

    // 1. Initial base config matching defaults
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

    // 2. Test recursive merge: toggling telemetry should preserve existing otlp.* settings
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

    // 3. Test validation: rejecting unknown telemetry / OTLP subkeys
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

    // 4. Test CLI dotted key config path parsing logic
    std::vector<std::string> cli_args = {
        "telemetry.otlp.endpoint=http://127.0.0.1:5555/v1/traces",
        "telemetry.otlp.protocol=http/json",
        "port=9090"
    };
    json cli_updates = parse_cli_args(cli_args);
    check(cli_updates["port"] == 9090, "CLI parses top-level key");
    check(cli_updates["telemetry"]["otlp"]["endpoint"] == "http://127.0.0.1:5555/v1/traces", "CLI parses 3-level dotted path endpoint");
    check(cli_updates["telemetry"]["otlp"]["protocol"] == "http/json", "CLI parses 3-level dotted path protocol");

    std::printf("================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    } else {
        std::printf("All C++ config/telemetry tests PASSED (%d passed).\n", passed);
        return 0;
    }
}
