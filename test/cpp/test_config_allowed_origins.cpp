#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <lemon/config_file.h>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::ConfigFile;
using lemon::RuntimeConfig;
using test_helpers::check;
using test_helpers::parse_cli_args;
using test_helpers::report_results;

static void set_env_var(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

static void clear_env_var(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

static bool set_throws(const json& cfg, const json& changes) {
    try {
        RuntimeConfig(cfg).set(changes);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

int main() {
    std::puts("=== RUNNING ALLOWED ORIGINS CONFIG TESTS ===");

    clear_env_var("LEMONADE_ALLOWED_ORIGINS");

    auto cfg_with = [](const std::string& origins) {
        return json::object({{"allowed_origins", origins}});
    };

    // 1. Default value from empty or missing key
    {
        RuntimeConfig rc_empty(json::object({}));
        check(rc_empty.allowed_origins() == "", "missing allowed_origins key defaults to empty string");

        RuntimeConfig rc_default(cfg_with(""));
        check(rc_default.allowed_origins() == "", "explicit empty string returns empty string");
    }

    // 2. Getter reads allowed_origins from config
    {
        RuntimeConfig rc(cfg_with("http://192.168.1.50:3000,http://dashboard.local:8080"));
        check(rc.allowed_origins() == "http://192.168.1.50:3000,http://dashboard.local:8080",
              "getter reads configured allowed_origins");

        RuntimeConfig rc_wildcard(cfg_with("*"));
        check(rc_wildcard.allowed_origins() == "*", "getter reads wildcard origin");
    }

    // 3. LEMONADE_ALLOWED_ORIGINS environment variable precedence
    {
        set_env_var("LEMONADE_ALLOWED_ORIGINS", "https://override.example.com");
        RuntimeConfig rc(cfg_with("http://configured.example.com"));
        check(rc.allowed_origins() == "https://override.example.com",
              "LEMONADE_ALLOWED_ORIGINS env var overrides config value");
        clear_env_var("LEMONADE_ALLOWED_ORIGINS");
    }

    // 4. validate()/set() accepts valid string values
    {
        check(!set_throws(cfg_with(""), json::object({{"allowed_origins", "http://localhost:3000"}})),
              "set accepts valid origin string");
        check(!set_throws(cfg_with("http://localhost:3000"), json::object({{"allowed_origins", "*"}})),
              "set accepts wildcard origin");
        check(!set_throws(cfg_with("http://localhost:3000"), json::object({{"allowed_origins", ""}})),
              "set accepts empty origin string");
    }

    // 5. validate()/set() rejects non-string values
    {
        check(set_throws(cfg_with(""), json::object({{"allowed_origins", 123}})),
              "rejects integer value");
        check(set_throws(cfg_with(""), json::object({{"allowed_origins", true}})),
              "rejects boolean value");
        check(set_throws(cfg_with(""), json::object({{"allowed_origins", json::array({"http://localhost:3000"})}})),
              "rejects array value");
        check(set_throws(cfg_with(""), json::object({{"allowed_origins", json::object({{"url", "http://localhost"}})}})),
              "rejects object value");
    }

    // 6. CLI argument parser handles allowed_origins
    {
        json cli_update = parse_cli_args({"allowed_origins=http://192.168.1.50:3000,http://dashboard.local:8080"});
        check(cli_update.contains("allowed_origins"), "CLI parser parses allowed_origins key");
        check(cli_update["allowed_origins"] == "http://192.168.1.50:3000,http://dashboard.local:8080",
              "CLI parser preserves comma-separated string");
    }

    // 7. Dynamic updates via set()
    {
        RuntimeConfig rc(cfg_with("http://initial.example.com"));
        check(rc.allowed_origins() == "http://initial.example.com", "initial origin");

        rc.set(json::object({{"allowed_origins", "http://updated.example.com"}}));
        check(rc.allowed_origins() == "http://updated.example.com", "updated origin applied");
        check(rc.snapshot()["allowed_origins"] == "http://updated.example.com", "snapshot updated");

        rc.set(json::object({{"allowed_origins", ""}}));
        check(rc.allowed_origins() == "", "clearing origin applied");

        // Runtime set overrides active environment variable for the session
        set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://env-active.example.com");
        RuntimeConfig rc_env(cfg_with("http://cfg-initial.example.com"));
        check(rc_env.allowed_origins() == "http://env-active.example.com", "env var takes initial precedence");
        rc_env.set(json::object({{"allowed_origins", "http://runtime-override.example.com"}}));
        check(rc_env.allowed_origins() == "http://runtime-override.example.com", "runtime set overrides active env var for session");
        clear_env_var("LEMONADE_ALLOWED_ORIGINS");
    }

    // 8. base_defaults() has allowed_origins
    {
        json defaults = ConfigFile::base_defaults();
        check(defaults.contains("allowed_origins"), "base_defaults() contains allowed_origins key");
        check(defaults["allowed_origins"] == "", "base_defaults() allowed_origins is empty string");
    }

    return report_results("allowed_origins config");
}
