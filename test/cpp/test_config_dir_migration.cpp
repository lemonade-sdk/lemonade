#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <lemon/utils/path_utils.h>

namespace fs = std::filesystem;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, std::string value)
        : name_(std::move(name)) {
        const char* old = std::getenv(name_.c_str());
        if (old) {
            had_old_value_ = true;
            old_value_ = old;
        }
        set(value.c_str());
    }

    ~ScopedEnvVar() {
        if (had_old_value_) {
            set(old_value_.c_str());
        } else {
#ifdef _WIN32
            SetEnvironmentVariableA(name_.c_str(), nullptr);
#else
            unsetenv(name_.c_str());
#endif
        }
    }

private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value);
#else
        setenv(name_.c_str(), value, 1);
#endif
    }

    std::string name_;
    std::string old_value_;
    bool had_old_value_ = false;
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    assert(out.good());
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    assert(in.good());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

void test_standard_cache_layout_migrates_to_explicit_config_dir() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_config_dir_migration_" + std::to_string(unique));
    const fs::path cache_dir = root / ".cache" / "lemonade";
    const fs::path config_dir = root / ".config" / "lemonade";

    try {
        write_text(cache_dir / "config.json", "{\"port\":13305}\n");
        write_text(cache_dir / "user_models.json", "{\"demo\":{}}\n");
        write_text(cache_dir / "recipe_options.json", "{\"demo\":{\"ctx_size\":4096}}\n");
        write_text(cache_dir / "mcp_servers.json", "{\"servers\":[]}\n");
        write_text(cache_dir / "jobs.json", "{\"jobs\":[]}\n");

        lemon::utils::migrate_legacy_json_files_to_config_dir(cache_dir.string(),
                                                              config_dir.string());

        for (const char* filename : {"config.json",
                                     "jobs.json",
                                     "mcp_servers.json",
                                     "recipe_options.json",
                                     "user_models.json"}) {
            const fs::path old_path = cache_dir / filename;
            const fs::path new_path = config_dir / filename;
            assert(!fs::exists(old_path));
            assert(fs::exists(new_path));
        }
        assert(read_text(config_dir / "config.json") == "{\"port\":13305}\n");
        assert(read_text(config_dir / "jobs.json") == "{\"jobs\":[]}\n");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}

void test_identical_cache_and_config_dirs_stay_put() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_config_dir_custom_" + std::to_string(unique));
    const fs::path custom_dir = root / "portable-data";

    try {
        write_text(custom_dir / "config.json", "{\"host\":\"127.0.0.1\"}\n");

        lemon::utils::migrate_legacy_json_files_to_config_dir(custom_dir.string(),
                                                              custom_dir.string());

        assert(fs::exists(custom_dir / "config.json"));
        assert(read_text(custom_dir / "config.json") == "{\"host\":\"127.0.0.1\"}\n");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}

void test_explicit_config_dir_is_honored() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_explicit_config_dir_" + std::to_string(unique));
    const fs::path cache_dir = root / "portable-cache";
    const fs::path config_dir = root / "portable-config";

    lemon::utils::set_cache_dir(cache_dir.string());
    lemon::utils::set_config_dir(config_dir.string());

    try {
        write_text(cache_dir / "config.json", "{\"host\":\"127.0.0.1\"}\n");

        assert(lemon::utils::get_cache_dir() == cache_dir.string());
        assert(lemon::utils::get_config_dir() == config_dir.string());

        lemon::utils::migrate_legacy_json_files_to_config_dir(cache_dir.string(),
                                                              config_dir.string());

        assert(!fs::exists(cache_dir / "config.json"));
        assert(fs::exists(config_dir / "config.json"));
        assert(read_text(config_dir / "config.json") == "{\"host\":\"127.0.0.1\"}\n");
    } catch (...) {
        lemon::utils::set_cache_dir("");
        lemon::utils::set_config_dir("");
        fs::remove_all(root);
        throw;
    }

    lemon::utils::set_cache_dir("");
    lemon::utils::set_config_dir("");
    fs::remove_all(root);
}

#ifndef _WIN32
void test_systemd_state_directory_takes_precedence_over_home() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_state_dir_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path state_dir = root / "var-lib-lemonade";

    ScopedEnvVar home_var("HOME", home.string());
    ScopedEnvVar state_var("STATE_DIRECTORY", state_dir.string());

    lemon::utils::set_config_dir("");

    const std::string resolved_config_dir = lemon::utils::get_config_dir();
    assert(resolved_config_dir == state_dir.string());

    fs::remove_all(root);
}

void test_systemd_cache_directory_is_used_directly() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_cache_dir_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path cache_dir = root / "var-cache-lemonade";

    ScopedEnvVar home_var("HOME", home.string());
    ScopedEnvVar cache_var("CACHE_DIRECTORY", cache_dir.string());

    lemon::utils::set_cache_dir("");

    const std::string resolved_cache_dir = lemon::utils::get_cache_dir();
    assert(resolved_cache_dir == cache_dir.string());

    fs::remove_all(root);
}

void test_systemd_relocation_recovers_cache_and_models() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_relocate_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path legacy_cache = home / ".cache" / "lemonade";
    const fs::path legacy_hub = home / ".cache" / "huggingface" / "hub";
    const fs::path cache_dir = root / "var" / "cache" / "lemonade";
    const fs::path config_dir = root / "var" / "lib" / "lemonade";

    ScopedEnvVar home_var("HOME", home.string());
    // Systemd exports these when it relocates the service's directories; the
    // recovery leg only runs when it sees them.
    ScopedEnvVar state_var("STATE_DIRECTORY", config_dir.string());
    ScopedEnvVar cache_var("CACHE_DIRECTORY", cache_dir.string());

    try {
        write_text(legacy_cache / "config.json", "{\"port\":9000}\n");
        write_text(legacy_cache / "bin" / "llama-server", "ELF\n");
        const fs::path legacy_model =
            legacy_hub / "models--demo--foo" / "snapshots" / "abc" / "model.gguf";
        write_text(legacy_model, "GGUF\n");

        lemon::utils::migrate_legacy_paths(cache_dir.string(), config_dir.string());

        assert(fs::exists(config_dir / "config.json"));
        assert(read_text(config_dir / "config.json") == "{\"port\":9000}\n");
        assert(!fs::exists(legacy_cache / "config.json"));

        assert(fs::exists(cache_dir / "bin" / "llama-server"));
        assert(read_text(cache_dir / "bin" / "llama-server") == "ELF\n");
        assert(!fs::exists(legacy_cache));

        // Models are deliberately left in place — no cross-filesystem copy.
        assert(fs::exists(legacy_model));
        assert(read_text(legacy_model) == "GGUF\n");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}

void test_relocation_preserves_existing_target_files() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_relocate_keep_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path legacy_cache = home / ".cache" / "lemonade";
    const fs::path cache_dir = root / "var" / "cache" / "lemonade";
    const fs::path config_dir = root / "var" / "lib" / "lemonade";

    ScopedEnvVar home_var("HOME", home.string());
    ScopedEnvVar state_var("STATE_DIRECTORY", config_dir.string());
    ScopedEnvVar cache_var("CACHE_DIRECTORY", cache_dir.string());

    try {
        write_text(legacy_cache / "config.json", "{\"stale\":true}\n");
        write_text(config_dir / "config.json", "{\"current\":true}\n");

        lemon::utils::migrate_legacy_paths(cache_dir.string(), config_dir.string());

        assert(read_text(config_dir / "config.json") == "{\"current\":true}\n");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}

// A user pointing lemond at an explicit cache dir (e.g. `lemond /tmp/scratch`)
// must NOT drain their real ~/.cache/lemonade — that only happens on a service
// upgrade, signalled by systemd's STATE_DIRECTORY / CACHE_DIRECTORY.
void test_explicit_cache_dir_does_not_drain_legacy() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_scratch_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path legacy_cache = home / ".cache" / "lemonade";
    const fs::path scratch = root / "scratch";

    ScopedEnvVar home_var("HOME", home.string());
    // No STATE_DIRECTORY / CACHE_DIRECTORY: not a service relocation.
    ScopedEnvVar state_var("STATE_DIRECTORY", "");
    ScopedEnvVar cache_var("CACHE_DIRECTORY", "");

    try {
        write_text(legacy_cache / "config.json", "{\"real\":true}\n");
        write_text(legacy_cache / "bin" / "llama-server", "ELF\n");

        // Portable/explicit invocation: cache_dir == config_dir == scratch.
        lemon::utils::migrate_legacy_paths(scratch.string(), scratch.string());

        // The real install is untouched; nothing was dragged into scratch.
        assert(fs::exists(legacy_cache / "config.json"));
        assert(read_text(legacy_cache / "config.json") == "{\"real\":true}\n");
        assert(fs::exists(legacy_cache / "bin" / "llama-server"));
        assert(!fs::exists(scratch / "config.json"));
        assert(!fs::exists(scratch / "bin"));
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}

void test_xdg_cache_and_config_dirs_are_resolved_separately() {
    const auto unique = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const fs::path root = fs::temp_directory_path() /
                          ("lemonade_config_dir_xdg_" + std::to_string(unique));
    const fs::path home = root / "home";
    const fs::path cache_home = root / "xdg-cache";
    const fs::path config_home = root / "xdg-config";
    const fs::path cache_dir = cache_home / "lemonade";
    const fs::path config_dir = config_home / "lemonade";

    ScopedEnvVar home_var("HOME", home.string());
    ScopedEnvVar cache_var("XDG_CACHE_HOME", cache_home.string());
    ScopedEnvVar config_var("XDG_CONFIG_HOME", config_home.string());

    try {
        write_text(cache_dir / "config.json", "{\"port\":13305}\n");

        assert(lemon::utils::get_cache_dir() == cache_dir.string());
        assert(lemon::utils::get_config_dir() == config_dir.string());
        lemon::utils::migrate_legacy_json_files_to_config_dir(cache_dir.string(),
                                                              config_dir.string());

        assert(!fs::exists(cache_dir / "config.json"));
        assert(fs::exists(config_dir / "config.json"));
        assert(read_text(config_dir / "config.json") == "{\"port\":13305}\n");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }

    fs::remove_all(root);
}
#endif

}

int main() {
    test_standard_cache_layout_migrates_to_explicit_config_dir();
    test_identical_cache_and_config_dirs_stay_put();
    test_explicit_config_dir_is_honored();
#ifndef _WIN32
    test_systemd_state_directory_takes_precedence_over_home();
    test_systemd_cache_directory_is_used_directly();
    test_systemd_relocation_recovers_cache_and_models();
    test_relocation_preserves_existing_target_files();
    test_explicit_cache_dir_does_not_drain_legacy();
    test_xdg_cache_and_config_dirs_are_resolved_separately();
#endif
    std::cout << "config dir migration tests passed\n";
    return 0;
}
