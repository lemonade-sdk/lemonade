#include "lemon/runtime_config.h"
#include "lemon/recipe_options.h"
#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace lemon {

const std::vector<std::string> RuntimeConfig::valid_log_levels_ = {
    "trace", "debug", "info", "warning", "error", "fatal", "none"
};

static bool is_recipe_key(const std::string& key) {
    static const auto keys = RecipeOptions::known_keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

RuntimeConfig::RuntimeConfig(int port,
                             const std::string& host,
                             const std::string& log_level,
                             const std::string& extra_models_dir,
                             bool no_broadcast,
                             long global_timeout,
                             int max_loaded_models,
                             const json& recipe_options)
    : config_({
        {"port", port},
        {"host", host},
        {"log_level", log_level},
        {"extra_models_dir", extra_models_dir},
        {"no_broadcast", no_broadcast},
        {"global_timeout", global_timeout},
        {"max_loaded_models", max_loaded_models}
      }) {
    // Merge recipe options into the flat config
    for (auto& [key, value] : recipe_options.items()) {
        config_[key] = value;
    }
}

// --- Getters ---

int RuntimeConfig::port() const {
    std::shared_lock lock(mutex_);
    return config_["port"].get<int>();
}

std::string RuntimeConfig::host() const {
    std::shared_lock lock(mutex_);
    return config_["host"].get<std::string>();
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

json RuntimeConfig::recipe_options() const {
    std::shared_lock lock(mutex_);
    json result = json::object();
    for (const auto& key : RecipeOptions::known_keys()) {
        if (config_.contains(key)) {
            result[key] = config_[key];
        }
    }
    return result;
}

// --- Validation ---

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
    } else if (key == "extra_models_dir") {
        if (!value.is_string()) {
            throw std::invalid_argument("'extra_models_dir' must be a string");
        }
    } else if (key == "no_broadcast") {
        if (!value.is_boolean()) {
            throw std::invalid_argument("'no_broadcast' must be a boolean");
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
            throw std::invalid_argument("'max_loaded_models' must be -1 (unlimited) or a positive integer");
        }
    } else if (is_recipe_key(key)) {
        // Recipe options: accept strings, numbers, and integers
        // (type enforcement is intentionally loose — RecipeOptions handles
        // per-recipe interpretation at model load time)
        if (!value.is_string() && !value.is_number()) {
            throw std::invalid_argument("'" + key + "' must be a string or number");
        }
    } else {
        throw std::invalid_argument("Unknown config key: '" + key + "'");
    }
}

// --- Unified setter ---

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

        for (auto& [key, value] : changes.items()) {
            if (!config_.contains(key) || config_[key] != value) {
                config_[key] = value;
                changed_keys.push_back(key);
            }
            updated[key] = value;
        }
    } // Lock released

    // Execute side effects outside the lock
    if (side_effect_cb && !changed_keys.empty()) {
        side_effect_cb(changed_keys);
    }

    return {{"status", "success"}, {"updated", updated}};
}

// --- Snapshot ---

json RuntimeConfig::snapshot() const {
    std::shared_lock lock(mutex_);
    return config_;
}

} // namespace lemon
