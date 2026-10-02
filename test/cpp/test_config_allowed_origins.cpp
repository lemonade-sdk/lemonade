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

    // 9. config_migrate_allowed_origins_env helper tests
    {
        json cfg_missing = json::object({{"broadcast", true}});
        check(lemon::config_migrate_allowed_origins_env(cfg_missing, "http://a.com"),
              "migrates when allowed_origins is missing");
        check(cfg_missing["allowed_origins"] == "http://a.com", "allowed_origins populated");

        json cfg_empty = json::object({{"allowed_origins", ""}});
        check(lemon::config_migrate_allowed_origins_env(cfg_empty, "http://b.com"),
              "migrates when allowed_origins is empty string");
        check(cfg_empty["allowed_origins"] == "http://b.com", "empty string replaced");

        json cfg_existing = json::object({{"allowed_origins", "http://existing.com"}});
        check(!lemon::config_migrate_allowed_origins_env(cfg_existing, "http://new.com"),
              "does not migrate when allowed_origins is non-empty");
        check(cfg_existing["allowed_origins"] == "http://existing.com", "existing value preserved");

        json cfg_null_env = json::object({{"allowed_origins", ""}});
        check(!lemon::config_migrate_allowed_origins_env(cfg_null_env, nullptr),
              "does not migrate when env var is null");
        check(!lemon::config_migrate_allowed_origins_env(cfg_null_env, ""),
              "does not migrate when env var is empty string");
    }

    // 10. ConfigFile::load() disk migration tests
    {
        namespace fs = std::filesystem;
        fs::path temp_dir = fs::temp_directory_path() / "lemonade_test_migration_origins";
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        // Case A: Fresh dir (no config.json) with env var set -> creates config.json with allowed_origins
        {
            fs::path dir_a = temp_dir / "case_a";
            fs::create_directories(dir_a);
            set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://migrated-fresh.example.com");

            json loaded = ConfigFile::load(dir_a.string(), dir_a.string());
            check(loaded["allowed_origins"] == "http://migrated-fresh.example.com",
                  "ConfigFile::load merged fresh config has migrated allowed_origins");

            json raw_on_disk = ConfigFile::load_raw(dir_a.string());
            check(raw_on_disk["allowed_origins"] == "http://migrated-fresh.example.com",
                  "ConfigFile::load created config.json on disk with migrated allowed_origins");
            clear_env_var("LEMONADE_ALLOWED_ORIGINS");
        }

        // Case B: Existing config.json without allowed_origins -> writes allowed_origins to disk
        {
            fs::path dir_b = temp_dir / "case_b";
            fs::create_directories(dir_b);
            ConfigFile::save(dir_b.string(), json::object({{"broadcast", true}}));

            set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://migrated-missing.example.com");
            json loaded = ConfigFile::load(dir_b.string(), dir_b.string());
            check(loaded["allowed_origins"] == "http://migrated-missing.example.com",
                  "ConfigFile::load updated missing field");

            json raw_on_disk = ConfigFile::load_raw(dir_b.string());
            check(raw_on_disk["allowed_origins"] == "http://migrated-missing.example.com",
                  "ConfigFile::load persisted migrated allowed_origins to existing config.json");
            clear_env_var("LEMONADE_ALLOWED_ORIGINS");
        }

        // Case C: Existing config.json with empty allowed_origins -> updates allowed_origins on disk
        {
            fs::path dir_c = temp_dir / "case_c";
            fs::create_directories(dir_c);
            ConfigFile::save(dir_c.string(), json::object({{"allowed_origins", ""}}));

            set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://migrated-empty.example.com");
            json loaded = ConfigFile::load(dir_c.string(), dir_c.string());
            check(loaded["allowed_origins"] == "http://migrated-empty.example.com",
                  "ConfigFile::load updated empty field");

            json raw_on_disk = ConfigFile::load_raw(dir_c.string());
            check(raw_on_disk["allowed_origins"] == "http://migrated-empty.example.com",
                  "ConfigFile::load persisted migrated allowed_origins to empty config.json");
            clear_env_var("LEMONADE_ALLOWED_ORIGINS");
        }

        // Case D: Existing config.json with non-empty allowed_origins (conflicting) -> does NOT overwrite on disk
        {
            fs::path dir_d = temp_dir / "case_d";
            fs::create_directories(dir_d);
            ConfigFile::save(dir_d.string(), json::object({{"allowed_origins", "http://keep-existing.example.com"}}));

            set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://should-not-overwrite.example.com");
            json loaded = ConfigFile::load(dir_d.string(), dir_d.string());
            json raw_on_disk = ConfigFile::load_raw(dir_d.string());
            check(raw_on_disk["allowed_origins"] == "http://keep-existing.example.com",
                  "ConfigFile::load does not overwrite existing non-empty allowed_origins on disk");
            clear_env_var("LEMONADE_ALLOWED_ORIGINS");
        }

        // Case E: Existing config.json with matching allowed_origins -> does not modify disk
        {
            fs::path dir_e = temp_dir / "case_e";
            fs::create_directories(dir_e);
            ConfigFile::save(dir_e.string(), json::object({{"allowed_origins", "http://matching.example.com"}}));

            set_env_var("LEMONADE_ALLOWED_ORIGINS", "http://matching.example.com");
            json loaded = ConfigFile::load(dir_e.string(), dir_e.string());
            json raw_on_disk = ConfigFile::load_raw(dir_e.string());
            check(raw_on_disk["allowed_origins"] == "http://matching.example.com",
                  "ConfigFile::load preserves matching allowed_origins on disk");
            clear_env_var("LEMONADE_ALLOWED_ORIGINS");
        }

        fs::remove_all(temp_dir);
    }

    return report_results("allowed_origins config");
}
