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
    test_xdg_cache_and_config_dirs_are_resolved_separately();
#endif
    std::cout << "config dir migration tests passed\n";
    return 0;
}
