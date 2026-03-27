#include "lemon/config_file.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace lemon {

std::shared_mutex ConfigFile::file_mutex_;

std::string ConfigFile::default_models_dir() {
    return utils::default_hf_cache_dir();
}

json ConfigFile::get_defaults() {
    return {
        {"config_version", 1},
        {"port", 8000},
        {"host", "localhost"},
        {"log_level", "info"},
        {"global_timeout", 300},
        {"max_loaded_models", 1},
        {"no_broadcast", false},
        {"extra_models_dir", ""},
        {"models_dir", default_models_dir()},
        {"ctx_size", 4096},
        {"offline", false},
        {"disable_model_filtering", false},
        {"enable_dgpu_gtt", false},
        {"llamacpp", {
            {"backend", "auto"},
            {"args", ""},
            {"prefer_system", false},
            {"rocm_bin", "builtin"},
            {"vulkan_bin", "builtin"},
            {"cpu_bin", "builtin"}
        }},
        {"whispercpp", {
            {"backend", "auto"},
            {"args", ""},
            {"cpu_bin", "builtin"},
            {"npu_bin", "builtin"}
        }},
        {"sdcpp", {
            {"backend", "auto"},
            {"steps", 20},
            {"cfg_scale", 7.0},
            {"width", 512},
            {"height", 512},
            {"cpu_bin", "builtin"},
            {"rocm_bin", "builtin"},
            {"vulkan_bin", "builtin"}
        }},
        {"flm", {
            {"args", ""},
            {"linux_beta", false}
        }},
        {"ryzenai", {
            {"server_bin", "builtin"}
        }},
        {"kokoro", {
            {"cpu_bin", "builtin"}
        }}
    };
}

json ConfigFile::load(const std::string& home_dir) {
    json defaults = get_defaults();
    fs::path config_path = utils::path_from_utf8(home_dir) / "config.json";

    if (!fs::exists(config_path)) {
        fs::path home_path = utils::path_from_utf8(home_dir);
        if (!fs::exists(home_path)) {
            fs::create_directories(home_path);
        }
        save(home_dir, defaults);
        return defaults;
    }

    // Read and parse config under shared lock
    bool corrupt = false;
    std::string parse_error_msg;
    json loaded;
    {
        std::shared_lock lock(file_mutex_);

        // Clean up stale temp file from a previous interrupted save
        std::error_code ec;
        fs::remove(fs::path(config_path).concat(".tmp"), ec);

        std::ifstream file(config_path);
        if (!file.is_open()) {
            std::cerr << "Warning: Could not open " << config_path.string()
                      << ", using defaults" << std::endl;
            return defaults;
        }

        try {
            loaded = json::parse(file);
        } catch (const json::parse_error& e) {
            corrupt = true;
            parse_error_msg = e.what();
        }
    } // shared lock released

    if (corrupt) {
        std::cerr << "Warning: Failed to parse " << config_path.string()
                  << ": " << parse_error_msg << std::endl;

        // Back up the corrupt file so the user can inspect it
        fs::path backup = config_path;
        backup += ".corrupted";
        std::error_code ec;
        fs::rename(config_path, backup, ec);
        if (!ec) {
            std::cerr << "  Renamed to " << backup.string() << std::endl;
        }

        std::cerr << "  Using defaults." << std::endl;
        save(home_dir, defaults);
        return defaults;
    }

    return utils::JsonUtils::merge(defaults, loaded);
}

void ConfigFile::save(const std::string& home_dir, const json& config) {
    std::unique_lock lock(file_mutex_);

    fs::path home_path = utils::path_from_utf8(home_dir);
    if (!fs::exists(home_path)) {
        fs::create_directories(home_path);
    }

    fs::path config_path = home_path / "config.json";
    fs::path temp_path = home_path / "config.json.tmp";

    {
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to write " + temp_path.string());
        }
        file << config.dump(2) << std::endl;
    }

    std::error_code ec;
    fs::rename(temp_path, config_path, ec);
    if (ec) {
        // On some systems (cross-device), rename fails. Fall back to copy + remove.
        std::error_code copy_ec;
        fs::copy_file(temp_path, config_path, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            fs::remove(temp_path);
            throw std::runtime_error("Failed to save " + config_path.string()
                                     + ": " + copy_ec.message());
        }
        fs::remove(temp_path);
    }
}

} // namespace lemon
