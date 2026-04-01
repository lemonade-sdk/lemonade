#include "lemon/config_file.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

std::shared_mutex ConfigFile::file_mutex_;

static json load_json_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open " + utils::path_to_utf8(path));
    }

    try {
        return json::parse(file);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse " + utils::path_to_utf8(path) + ": " + e.what());
    }
}

json ConfigFile::get_defaults() {
    json defaults = load_json_file(utils::path_from_utf8(
        utils::get_resource_path("resources/defaults.json")));

#ifndef _WIN32
    fs::path distro_defaults = "/usr/share/lemonade/defaults.json";
    if (fs::exists(distro_defaults)) {
        defaults = utils::JsonUtils::merge(defaults, load_json_file(distro_defaults));
    }
#endif

    return defaults;
}

json ConfigFile::load(const std::string& cache_dir) {
    json defaults = get_defaults();
    fs::path config_path = utils::path_from_utf8(cache_dir) / "config.json";

    if (!fs::exists(config_path)) {
        fs::path cache_path = utils::path_from_utf8(cache_dir);
        if (!fs::exists(cache_path)) {
            fs::create_directories(cache_path);
        }
        json config = migrate_from_env(defaults);
        save(cache_dir, config);
        return config;
    }

    // Clean up stale temp file from a previous interrupted save
    {
        std::unique_lock lock(file_mutex_);
        std::error_code ec;
        fs::remove(fs::path(config_path).concat(".tmp"), ec);
    }

    // Read and parse config under shared lock
    bool corrupt = false;
    std::string parse_error_msg;
    json loaded;
    {
        std::shared_lock lock(file_mutex_);

        std::ifstream file(config_path);
        if (!file.is_open()) {
            LOG(WARNING) << "Could not open " << config_path.string()
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
        LOG(WARNING) << "Failed to parse " << config_path.string()
                     << ": " << parse_error_msg << std::endl;

        // Back up the corrupt file so the user can inspect it
        fs::path backup = config_path;
        backup += ".corrupted";
        std::error_code ec;
        fs::rename(config_path, backup, ec);
        if (!ec) {
            LOG(WARNING) << "  Renamed to " << backup.string() << std::endl;
        }

        LOG(WARNING) << "  Using defaults." << std::endl;
        save(cache_dir, defaults);
        return defaults;
    }

    return utils::JsonUtils::merge(defaults, loaded);
}

void ConfigFile::save(const std::string& cache_dir, const json& config) {
    std::unique_lock lock(file_mutex_);

    fs::path cache_path = utils::path_from_utf8(cache_dir);
    if (!fs::exists(cache_path)) {
        fs::create_directories(cache_path);
    }

    fs::path config_path = cache_path / "config.json";
    fs::path temp_path = cache_path / "config.json.tmp";

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

// ---------------------------------------------------------------------------
// Environment-variable migration
// ---------------------------------------------------------------------------

struct EnvMapping {
    const char* env_name;
    const char* top_key;
    const char* nested_key; // nullptr for top-level keys
};

static const EnvMapping env_mappings[] = {
    // Top-level settings
    {"LEMONADE_PORT",                    "port",                     nullptr},
    {"LEMONADE_HOST",                    "host",                     nullptr},
    {"LEMONADE_LOG_LEVEL",               "log_level",                nullptr},
    {"LEMONADE_GLOBAL_TIMEOUT",          "global_timeout",           nullptr},
    {"LEMONADE_MAX_LOADED_MODELS",       "max_loaded_models",        nullptr},
    {"LEMONADE_NO_BROADCAST",            "no_broadcast",             nullptr},
    {"LEMONADE_EXTRA_MODELS_DIR",        "extra_models_dir",         nullptr},
    {"LEMONADE_CTX_SIZE",                "ctx_size",                 nullptr},
    {"LEMONADE_OFFLINE",                 "offline",                  nullptr},
    {"LEMONADE_DISABLE_MODEL_FILTERING", "disable_model_filtering",  nullptr},
    {"LEMONADE_ENABLE_DGPU_GTT",         "enable_dgpu_gtt",          nullptr},
    // llamacpp
    {"LEMONADE_LLAMACPP",                "llamacpp",  "backend"},
    {"LEMONADE_LLAMACPP_ARGS",           "llamacpp",  "args"},
    {"LEMONADE_LLAMACPP_PREFER_SYSTEM",  "llamacpp",  "prefer_system"},
    {"LEMONADE_LLAMACPP_ROCM_BIN",       "llamacpp",  "rocm_bin"},
    {"LEMONADE_LLAMACPP_VULKAN_BIN",     "llamacpp",  "vulkan_bin"},
    {"LEMONADE_LLAMACPP_CPU_BIN",        "llamacpp",  "cpu_bin"},
    // whispercpp
    {"LEMONADE_WHISPERCPP",              "whispercpp", "backend"},
    {"LEMONADE_WHISPERCPP_ARGS",         "whispercpp", "args"},
    {"LEMONADE_WHISPERCPP_CPU_BIN",      "whispercpp", "cpu_bin"},
    {"LEMONADE_WHISPERCPP_NPU_BIN",      "whispercpp", "npu_bin"},
    // sdcpp
    {"LEMONADE_SDCPP",                   "sdcpp", "backend"},
    {"LEMONADE_SDCPP_ARGS",              "sdcpp", "args"},
    {"LEMONADE_STEPS",                   "sdcpp", "steps"},
    {"LEMONADE_CFG_SCALE",               "sdcpp", "cfg_scale"},
    {"LEMONADE_WIDTH",                   "sdcpp", "width"},
    {"LEMONADE_HEIGHT",                  "sdcpp", "height"},
    {"LEMONADE_SDCPP_CPU_BIN",           "sdcpp", "cpu_bin"},
    {"LEMONADE_SDCPP_ROCM_BIN",          "sdcpp", "rocm_bin"},
    {"LEMONADE_SDCPP_VULKAN_BIN",        "sdcpp", "vulkan_bin"},
    // flm
    {"LEMONADE_FLM_ARGS",               "flm", "args"},
    // ryzenai
    {"LEMONADE_RYZENAI_SERVER_BIN",      "ryzenai", "server_bin"},
    // kokoro
    {"LEMONADE_KOKORO_CPU_BIN",          "kokoro", "cpu_bin"},
};

/// Parse a string value to match the JSON type of the corresponding default.
static json parse_env_value(const std::string& value, const json& default_val) {
    if (default_val.is_boolean()) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return (lower == "1" || lower == "true" || lower == "yes");
    }
    if (default_val.is_number_integer()) {
        try { return std::stoi(value); } catch (...) { return default_val; }
    }
    if (default_val.is_number_float()) {
        try { return std::stod(value); } catch (...) { return default_val; }
    }
    return value;
}

json ConfigFile::migrate_from_env(const json& defaults) {
    json overlay = json::object();
    std::vector<std::string> migrated;

    for (const auto& m : env_mappings) {
        std::string val = utils::get_environment_variable_utf8(m.env_name);
        if (val.empty()) continue;

        // Look up the default to determine the expected type
        json default_val;
        if (m.nested_key == nullptr) {
            if (defaults.contains(m.top_key)) default_val = defaults[m.top_key];
        } else {
            if (defaults.contains(m.top_key) && defaults[m.top_key].contains(m.nested_key))
                default_val = defaults[m.top_key][m.nested_key];
        }

        json parsed = parse_env_value(val, default_val);

        if (m.nested_key == nullptr) {
            overlay[m.top_key] = parsed;
        } else {
            if (!overlay.contains(m.top_key)) overlay[m.top_key] = json::object();
            overlay[m.top_key][m.nested_key] = parsed;
        }

        migrated.push_back(m.env_name);
    }

    if (!migrated.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < migrated.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << migrated[i];
        }
        LOG(INFO) << "Migrated config.json from environment variables: "
                  << oss.str() << std::endl;
    }

    return utils::JsonUtils::merge(defaults, overlay);
}

} // namespace lemon
