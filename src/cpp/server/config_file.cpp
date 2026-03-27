#include "lemon/config_file.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

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

std::pair<std::string, std::string> ConfigFile::env_to_config_key(const std::string& env_name) {
    if (env_name == "LEMONADE_PORT") return {"port", ""};
    if (env_name == "LEMONADE_HOST") return {"host", ""};
    if (env_name == "LEMONADE_LOG_LEVEL") return {"log_level", ""};
    if (env_name == "LEMONADE_GLOBAL_TIMEOUT") return {"global_timeout", ""};
    if (env_name == "LEMONADE_MAX_LOADED_MODELS") return {"max_loaded_models", ""};
    if (env_name == "LEMONADE_NO_BROADCAST") return {"no_broadcast", ""};
    if (env_name == "LEMONADE_EXTRA_MODELS_DIR") return {"extra_models_dir", ""};
    if (env_name == "LEMONADE_CTX_SIZE") return {"ctx_size", ""};
    if (env_name == "LEMONADE_OFFLINE") return {"offline", ""};
    if (env_name == "LEMONADE_DISABLE_MODEL_FILTERING") return {"disable_model_filtering", ""};
    if (env_name == "LEMONADE_ENABLE_DGPU_GTT") return {"enable_dgpu_gtt", ""};

    if (env_name == "LEMONADE_LLAMACPP") return {"llamacpp", "backend"};
    if (env_name == "LEMONADE_LLAMACPP_ARGS") return {"llamacpp", "args"};
    if (env_name == "LEMONADE_LLAMACPP_PREFER_SYSTEM") return {"llamacpp", "prefer_system"};
    if (env_name == "LEMONADE_LLAMACPP_ROCM_BIN") return {"llamacpp", "rocm_bin"};
    if (env_name == "LEMONADE_LLAMACPP_VULKAN_BIN") return {"llamacpp", "vulkan_bin"};
    if (env_name == "LEMONADE_LLAMACPP_CPU_BIN") return {"llamacpp", "cpu_bin"};

    if (env_name == "LEMONADE_WHISPERCPP") return {"whispercpp", "backend"};
    if (env_name == "LEMONADE_WHISPERCPP_ARGS") return {"whispercpp", "args"};
    if (env_name == "LEMONADE_WHISPERCPP_CPU_BIN") return {"whispercpp", "cpu_bin"};
    if (env_name == "LEMONADE_WHISPERCPP_NPU_BIN") return {"whispercpp", "npu_bin"};

    if (env_name == "LEMONADE_SDCPP") return {"sdcpp", "backend"};
    if (env_name == "LEMONADE_STEPS") return {"sdcpp", "steps"};
    if (env_name == "LEMONADE_CFG_SCALE") return {"sdcpp", "cfg_scale"};
    if (env_name == "LEMONADE_WIDTH") return {"sdcpp", "width"};
    if (env_name == "LEMONADE_HEIGHT") return {"sdcpp", "height"};
    if (env_name == "LEMONADE_SDCPP_CPU_BIN") return {"sdcpp", "cpu_bin"};
    if (env_name == "LEMONADE_SDCPP_ROCM_BIN") return {"sdcpp", "rocm_bin"};
    if (env_name == "LEMONADE_SDCPP_VULKAN_BIN") return {"sdcpp", "vulkan_bin"};

    if (env_name == "LEMONADE_FLM_ARGS") return {"flm", "args"};
    if (env_name == "LEMONADE_FLM_LINUX_BETA") return {"flm", "linux_beta"};

    if (env_name == "LEMONADE_RYZENAI_SERVER_BIN") return {"ryzenai", "server_bin"};

    if (env_name == "LEMONADE_KOKORO_CPU_BIN") return {"kokoro", "cpu_bin"};

    return {"", ""};
}

json ConfigFile::read_conf_file(const std::string& path) {
    json result = json::object();
    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        // Find KEY=VALUE
        auto eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Trim whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());

        // Remove surrounding quotes from value
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty() && !value.empty()) {
            result[key] = value;
        }
    }

    return result;
}

/// Try to parse a string value as the appropriate JSON type for a given config key.
static json parse_env_value(const std::string& value, const std::string& top_key,
                            const std::string& nested_key, const json& defaults) {
    // Look up the default to determine expected type
    json default_val;
    if (nested_key.empty()) {
        if (defaults.contains(top_key)) {
            default_val = defaults[top_key];
        }
    } else {
        if (defaults.contains(top_key) && defaults[top_key].contains(nested_key)) {
            default_val = defaults[top_key][nested_key];
        }
    }

    // Boolean: "1", "true", "yes" -> true; "0", "false", "no" -> false
    if (default_val.is_boolean()) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return (lower == "1" || lower == "true" || lower == "yes");
    }

    // Integer
    if (default_val.is_number_integer()) {
        try { return std::stoi(value); } catch (...) {}
    }

    // Float
    if (default_val.is_number_float()) {
        try { return std::stod(value); } catch (...) {}
    }

    // String (default)
    return value;
}

void ConfigFile::migrate(const std::string& home_dir) {
    fs::path config_path = utils::path_from_utf8(home_dir) / "config.json";
    if (fs::exists(config_path)) {
        return;  // config.json already exists, nothing to migrate
    }

    json defaults = get_defaults();
    json migrated = json::object();
    std::vector<std::string> migrated_sources;

    static const std::vector<std::string> env_names = {
        "LEMONADE_PORT", "LEMONADE_HOST", "LEMONADE_LOG_LEVEL",
        "LEMONADE_GLOBAL_TIMEOUT", "LEMONADE_MAX_LOADED_MODELS",
        "LEMONADE_NO_BROADCAST", "LEMONADE_EXTRA_MODELS_DIR",
        "LEMONADE_CTX_SIZE", "LEMONADE_OFFLINE",
        "LEMONADE_DISABLE_MODEL_FILTERING", "LEMONADE_ENABLE_DGPU_GTT",
        "LEMONADE_LLAMACPP", "LEMONADE_LLAMACPP_ARGS",
        "LEMONADE_LLAMACPP_PREFER_SYSTEM",
        "LEMONADE_LLAMACPP_ROCM_BIN", "LEMONADE_LLAMACPP_VULKAN_BIN",
        "LEMONADE_LLAMACPP_CPU_BIN",
        "LEMONADE_WHISPERCPP", "LEMONADE_WHISPERCPP_ARGS",
        "LEMONADE_WHISPERCPP_CPU_BIN", "LEMONADE_WHISPERCPP_NPU_BIN",
        "LEMONADE_SDCPP", "LEMONADE_STEPS", "LEMONADE_CFG_SCALE",
        "LEMONADE_WIDTH", "LEMONADE_HEIGHT",
        "LEMONADE_SDCPP_CPU_BIN", "LEMONADE_SDCPP_ROCM_BIN",
        "LEMONADE_SDCPP_VULKAN_BIN",
        "LEMONADE_FLM_ARGS", "LEMONADE_FLM_LINUX_BETA",
        "LEMONADE_RYZENAI_SERVER_BIN", "LEMONADE_KOKORO_CPU_BIN"
    };

    // 1. Try reading /etc/lemonade/lemonade.conf (Linux only)
#ifdef __linux__
    json conf_values = read_conf_file("/etc/lemonade/lemonade.conf");
    if (!conf_values.empty()) {
        migrated_sources.push_back("/etc/lemonade/lemonade.conf");
        for (auto& [env_name, env_val] : conf_values.items()) {
            auto [top_key, nested_key] = env_to_config_key(env_name);
            if (top_key.empty()) continue;

            std::string val = env_val.get<std::string>();
            json parsed = parse_env_value(val, top_key, nested_key, defaults);

            if (nested_key.empty()) {
                migrated[top_key] = parsed;
            } else {
                if (!migrated.contains(top_key)) migrated[top_key] = json::object();
                migrated[top_key][nested_key] = parsed;
            }
        }
    }
#endif

    // 2. Check environment variables (override conf file values)
    for (const auto& env_name : env_names) {
        std::string val = utils::get_environment_variable_utf8(env_name);
        if (val.empty()) continue;

        auto [top_key, nested_key] = env_to_config_key(env_name);
        if (top_key.empty()) continue;

        json parsed = parse_env_value(val, top_key, nested_key, defaults);

        if (nested_key.empty()) {
            migrated[top_key] = parsed;
        } else {
            if (!migrated.contains(top_key)) migrated[top_key] = json::object();
            migrated[top_key][nested_key] = parsed;
        }

        migrated_sources.push_back(env_name + " env var");
    }

    if (!migrated_sources.empty()) {
        // Log what was migrated
        std::cerr << "Migrated configuration to config.json from: ";
        for (size_t i = 0; i < migrated_sources.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << migrated_sources[i];
        }
        std::cerr << std::endl;
    }

    // If we migrated values, merge them into defaults and save config.json
    if (!migrated.empty()) {
        json config = utils::JsonUtils::merge(defaults, migrated);

        // Ensure home dir exists
        fs::path home_path = utils::path_from_utf8(home_dir);
        if (!fs::exists(home_path)) {
            fs::create_directories(home_path);
        }

        save(home_dir, config);
    }
}

} // namespace lemon
