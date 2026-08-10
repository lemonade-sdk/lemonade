#include <cstdio>
#include <stdexcept>
#include <string>
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

int main() {
    // Absent key falls back to Hugging Face for backward compatibility.
    {
        RuntimeConfig config(json::object());
        check(config.default_model_source() == "huggingface",
              "missing default_model_source falls back to huggingface");
    }

    // Explicit huggingface round-trips.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        check(config.default_model_source() == "huggingface",
              "explicit huggingface preserved");
    }

    // Switching the policy to modelscope is accepted and observable.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        config.set(json{{"default_model_source", "modelscope"}});
        check(config.default_model_source() == "modelscope",
              "default_model_source set to modelscope");
        check(config.snapshot()["default_model_source"] == "modelscope",
              "modelscope reflected in snapshot");
    }

    // Unknown registry names are rejected.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        bool threw = false;
        try {
            config.set(json{{"default_model_source", "nexus"}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "rejects unsupported default_model_source value");
        check(config.default_model_source() == "huggingface",
              "rejected value leaves prior policy intact");
    }

    // Non-string values are rejected.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        bool threw = false;
        try {
            config.set(json{{"default_model_source", 42}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "rejects non-string default_model_source value");
    }

    std::printf("================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("All default_model_source tests PASSED (%d passed).\n", passed);
    return 0;
}
