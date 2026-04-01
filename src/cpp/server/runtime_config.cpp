#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lemon {

const std::vector<std::string> RuntimeConfig::valid_log_levels_ = {
    "trace", "debug", "info", "warning", "error", "fatal", "none"
};

// Global instance pointer (set once at startup, read from any thread after)
static std::atomic<RuntimeConfig*> s_global_instance{nullptr};

void RuntimeConfig::set_global(RuntimeConfig* instance) {
    s_global_instance.store(instance, std::memory_order_release);
}

RuntimeConfig* RuntimeConfig::global() {
    return s_global_instance.load(std::memory_order_acquire);
}

static const std::vector<std::string> s_backend_names = {
    "llamacpp", "whispercpp", "sdcpp", "flm", "ryzenai", "kokoro"
};

static bool is_backend_name(const std::string& key) {
    return std::find(s_backend_names.begin(), s_backend_names.end(), key) != s_backend_names.end();
}

// Backends that have a selectable "backend" key
static const std::vector<std::string> s_selectable_backends = {
    "llamacpp", "whispercpp", "sdcpp"
};

static bool has_backend_selection(const std::string& config_section) {
    return std::find(s_selectable_backends.begin(), s_selectable_backends.end(),
                     config_section) != s_selectable_backends.end();
}

std::string RuntimeConfig::config_section_to_recipe(const std::string& config_section) {
    if (config_section == "sdcpp") return "sd-cpp";
    return config_section;
}

std::string RuntimeConfig::recipe_to_config_section(const std::string& recipe) {
    if (recipe == "sd-cpp") return "sdcpp";
    return recipe;
}

void RuntimeConfig::validate_backend_choice(const std::string& config_section,
                                             const std::string& value) {
    if (value == "auto") return;

    if (!has_backend_selection(config_section)) {
        throw std::invalid_argument(
            "'" + config_section + "' does not have a selectable backend");
    }

    std::string recipe = config_section_to_recipe(config_section);
    auto result = SystemInfo::get_supported_backends(recipe);

    if (std::find(result.backends.begin(), result.backends.end(), value)
        == result.backends.end()) {
        std::string allowed = "auto";
        for (const auto& b : result.backends) {
            allowed += ", " + b;
        }
        throw std::invalid_argument(
            "'" + config_section + ".backend' must be one of: " + allowed);
    }
}

void RuntimeConfig::validate_bin_path(const std::string& config_section,
                                       const std::string& key,
                                       const std::string& value) {
    if (value.empty() || value == "builtin") return;

    if (!fs::exists(value)) {
        throw std::invalid_argument(
            "'" + config_section + "." + key + "' path does not exist: " + value
            + ". Set to \"builtin\" to use the default binary.");
    }
}

RuntimeConfig::RuntimeConfig(const json& config)
    : config_(config) {
    // Config is expected to already have defaults merged in (by ConfigFile::load).

    // In CI mode, override log level to debug for easier diagnostics
    const char* ci_mode = std::getenv("LEMONADE_CI_MODE");
    if (ci_mode && (std::string(ci_mode) == "1" || std::string(ci_mode) == "true" ||
                    std::string(ci_mode) == "True" || std::string(ci_mode) == "TRUE")) {
        config_["log_level"] = "debug";
    }
}

int RuntimeConfig::port() const {
    std::shared_lock lock(mutex_);
    return config_["port"].get<int>();
}

std::string RuntimeConfig::host() const {
    std::shared_lock lock(mutex_);
    return config_["host"].get<std::string>();
}

int RuntimeConfig::websocket_port() const {
    std::shared_lock lock(mutex_);
    const auto& val = config_["websocket_port"];
    if (val.is_string() && val.get<std::string>() == "auto") {
        return 0;  // OS auto-assign
    }
    return val.get<int>();
}

std::string RuntimeConfig::log_level() const {
    std::shared_lock lock(mutex_);
    return config_["log_level"].get<std::string>();
}

std::string RuntimeConfig::extra_models_dir() const {
    std::shared_lock lock(mutex_);
    return config_["extra_models_dir"].get<std::string>();
}

bool RuntimeConfig::no_broadcast() const {
    std::shared_lock lock(mutex_);
    return config_["no_broadcast"].get<bool>();
}

long RuntimeConfig::global_timeout() const {
    std::shared_lock lock(mutex_);
    return config_["global_timeout"].get<long>();
}

int RuntimeConfig::max_loaded_models() const {
    std::shared_lock lock(mutex_);
    return config_["max_loaded_models"].get<int>();
}

std::string RuntimeConfig::models_dir() const {
    std::shared_lock lock(mutex_);
    return config_["models_dir"].get<std::string>();
}

int RuntimeConfig::ctx_size() const {
    std::shared_lock lock(mutex_);
    return config_["ctx_size"].get<int>();
}

bool RuntimeConfig::offline() const {
    std::shared_lock lock(mutex_);
    return config_["offline"].get<bool>();
}

bool RuntimeConfig::disable_model_filtering() const {
    std::shared_lock lock(mutex_);
    return config_["disable_model_filtering"].get<bool>();
}

bool RuntimeConfig::enable_dgpu_gtt() const {
    std::shared_lock lock(mutex_);
    return config_["enable_dgpu_gtt"].get<bool>();
}

json RuntimeConfig::backend_config(const std::string& backend_name) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend_name) && config_[backend_name].is_object()) {
        return config_[backend_name];
    }
    return json::object();
}

std::string RuntimeConfig::backend_string(const std::string& backend,
                                           const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<std::string>();
    }
    return "";
}

bool RuntimeConfig::backend_bool(const std::string& backend,
                                  const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<bool>();
    }
    return false;
}

int RuntimeConfig::backend_int(const std::string& backend,
                                const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<int>();
    }
    return 0;
}

double RuntimeConfig::backend_double(const std::string& backend,
                                      const std::string& key) const {
    std::shared_lock lock(mutex_);
    if (config_.contains(backend) && config_[backend].contains(key)) {
        return config_[backend][key].get<double>();
    }
    return 0.0;
}

json RuntimeConfig::recipe_options() const {
    std::shared_lock lock(mutex_);
    json result = json::object();

    // "auto" in config.json means auto-detect; the flat recipe_options format
    // uses "" (empty string) for auto-detect, so we translate here.
    auto resolve_auto = [](const json& val) -> json {
        if (val.is_string() && val.get<std::string>() == "auto") return "";
        return val;
    };

    if (config_.contains("llamacpp")) {
        const auto& lc = config_["llamacpp"];
        if (lc.contains("backend")) result["llamacpp_backend"] = resolve_auto(lc["backend"]);
        if (lc.contains("args")) result["llamacpp_args"] = lc["args"];
    }

    if (config_.contains("whispercpp")) {
        const auto& wc = config_["whispercpp"];
        if (wc.contains("backend")) result["whispercpp_backend"] = resolve_auto(wc["backend"]);
        if (wc.contains("args")) result["whispercpp_args"] = wc["args"];
    }

    if (config_.contains("sdcpp")) {
        const auto& sd = config_["sdcpp"];
        if (sd.contains("backend")) result["sd-cpp_backend"] = resolve_auto(sd["backend"]);
        if (sd.contains("args")) result["sdcpp_args"] = sd["args"];
        if (sd.contains("steps")) result["steps"] = sd["steps"];
        if (sd.contains("cfg_scale")) result["cfg_scale"] = sd["cfg_scale"];
        if (sd.contains("width")) result["width"] = sd["width"];
        if (sd.contains("height")) result["height"] = sd["height"];
    }

    if (config_.contains("flm")) {
        const auto& flm = config_["flm"];
        if (flm.contains("args")) result["flm_args"] = flm["args"];
    }

    if (config_.contains("ctx_size")) result["ctx_size"] = config_["ctx_size"];

    return result;
}

void RuntimeConfig::validate(const std::string& key, const json& value) const {
    if (key == "port") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'port' must be an integer");
        }
        int p = value.get<int>();
        if (p < 1 || p > 65535) {
            throw std::invalid_argument("'port' must be between 1 and 65535");
        }
    } else if (key == "host") {
        if (!value.is_string()) {
            throw std::invalid_argument("'host' must be a string");
        }
    } else if (key == "websocket_port") {
        if (value.is_string()) {
            if (value.get<std::string>() != "auto") {
                throw std::invalid_argument(
                    "'websocket_port' must be \"auto\" or an integer 0-65535");
            }
        } else if (value.is_number_integer()) {
            int p = value.get<int>();
            if (p < 0 || p > 65535) {
                throw std::invalid_argument(
                    "'websocket_port' must be between 0 and 65535");
            }
        } else {
            throw std::invalid_argument(
                "'websocket_port' must be \"auto\" or an integer 0-65535");
        }
    } else if (key == "log_level") {
        if (!value.is_string()) {
            throw std::invalid_argument("'log_level' must be a string");
        }
        std::string level = value.get<std::string>();
        if (std::find(valid_log_levels_.begin(), valid_log_levels_.end(), level)
            == valid_log_levels_.end()) {
            throw std::invalid_argument(
                "'log_level' must be one of: trace, debug, info, warning, error, fatal, none");
        }
    } else if (key == "extra_models_dir" || key == "models_dir") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + key + "' must be a string");
        }
    } else if (key == "no_broadcast" || key == "offline" ||
               key == "disable_model_filtering" || key == "enable_dgpu_gtt") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'" + key + "' must be a boolean");
        }
    } else if (key == "global_timeout") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'global_timeout' must be an integer");
        }
        if (value.get<long>() <= 0) {
            throw std::invalid_argument("'global_timeout' must be positive");
        }
    } else if (key == "max_loaded_models") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'max_loaded_models' must be an integer");
        }
        int m = value.get<int>();
        if (m < -1 || m == 0) {
            throw std::invalid_argument(
                "'max_loaded_models' must be -1 (unlimited) or a positive integer");
        }
    } else if (key == "ctx_size") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'ctx_size' must be an integer");
        }
        if (value.get<int>() <= 0) {
            throw std::invalid_argument("'ctx_size' must be positive");
        }
    } else if (key == "config_version") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'config_version' must be an integer");
        }
        if (value.get<int>() < 1) {
            throw std::invalid_argument("'config_version' must be >= 1");
        }
    } else if (is_backend_name(key)) {
        if (!value.is_object()) {
            throw std::invalid_argument("'" + key + "' must be an object");
        }
        // Validate each sub-key
        for (auto& [sub_key, sub_value] : value.items()) {
            validate_backend(key, sub_key, sub_value);
        }
    } else {
        throw std::invalid_argument("Unknown config key: '" + key + "'");
    }
}

void RuntimeConfig::validate_backend(const std::string& backend, const std::string& key,
                                      const json& value) const {
    if (key == "backend") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
        validate_backend_choice(backend, value.get<std::string>());
    }
    else if (key == "args") {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
    }
    else if (key.find("_bin") != std::string::npos) {
        if (!value.is_string()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a string");
        }
        validate_bin_path(backend, key, value.get<std::string>());
    }
    else if (key == "prefer_system") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a boolean");
        }
    }
    else if (key == "steps" || key == "width" || key == "height") {
        if (!value.is_number_integer()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be an integer");
        }
        if (value.get<int>() <= 0) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be positive");
        }
    }
    else if (key == "cfg_scale") {
        if (!value.is_number()) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be a number");
        }
        if (value.get<double>() <= 0.0) {
            throw std::invalid_argument("'" + backend + "." + key + "' must be positive");
        }
    }
    else {
        throw std::invalid_argument("Unknown key: '" + backend + "." + key + "'");
    }
}

void RuntimeConfig::apply_changes(const json& changes, std::vector<std::string>& changed_keys) {
    for (auto& [key, value] : changes.items()) {
        if (value.is_object() && is_backend_name(key)) {
            // Merge nested backend changes
            if (!config_.contains(key)) {
                config_[key] = json::object();
            }
            for (auto& [sub_key, sub_value] : value.items()) {
                if (!config_[key].contains(sub_key) || config_[key][sub_key] != sub_value) {
                    config_[key][sub_key] = sub_value;
                    changed_keys.push_back(key);  // Track that this backend section changed
                }
            }
        } else {
            if (!config_.contains(key) || config_[key] != value) {
                config_[key] = value;
                changed_keys.push_back(key);
            }
        }
    }

    // Deduplicate changed_keys
    std::sort(changed_keys.begin(), changed_keys.end());
    changed_keys.erase(std::unique(changed_keys.begin(), changed_keys.end()),
                       changed_keys.end());
}

json RuntimeConfig::set(const json& changes, ConfigSideEffectCallback side_effect_cb) {
    if (!changes.is_object() || changes.empty()) {
        throw std::invalid_argument("Request body must be a non-empty JSON object");
    }

    // Validate all keys before acquiring write lock
    for (auto& [key, value] : changes.items()) {
        validate(key, value);
    }

    std::vector<std::string> changed_keys;
    json updated = json::object();

    {
        std::unique_lock lock(mutex_);
        apply_changes(changes, changed_keys);

        // Build updated response
        for (auto& [key, value] : changes.items()) {
            updated[key] = value;
        }
    } // Lock released

    // Execute side effects outside the lock
    if (side_effect_cb && !changed_keys.empty()) {
        side_effect_cb(changed_keys);
    }

    return {{"status", "success"}, {"updated", updated}};
}

json RuntimeConfig::snapshot() const {
    std::shared_lock lock(mutex_);
    return config_;
}

} // namespace lemon
