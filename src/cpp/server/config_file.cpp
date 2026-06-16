#include "lemon/config_file.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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

    // Packagers on non-FHS distros (Nix, Guix) can't write the /usr/share
    // file above; this seeds the same defaults from any path.
    if (const char* env = std::getenv("LEMONADE_DEFAULTS_PATH"); env && *env && fs::exists(env)) {
        defaults = utils::JsonUtils::merge(defaults, load_json_file(env));
    }

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
        save(cache_dir, defaults);
        return defaults;
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

    // Deep-merge: user values override defaults, missing fields filled from defaults.
    json merged = utils::JsonUtils::merge(defaults, loaded);

    // Capture the original config version BEFORE merge, so that migration
    // can see past the defaults-injected version number.
    int original_version = config_get_version(loaded);

    // Apply migrations if the config is older than the current version.
    // The inline config_migrate() handles version bumping and field removal.
    bool migrated = config_migrate(merged, defaults, original_version);
    if (migrated) {
        // Log migration details for user visibility.
        if (original_version < config_get_version(defaults)) {
            if (loaded.contains("ctx_size") && loaded["ctx_size"].is_number_integer()
                && loaded["ctx_size"].get<int>() == 4096) {
                LOG(INFO) << "Migrating config: ctx_size 4096 -> -1 (auto-tune enabled)"
                          << std::endl;
            }
        }
        save(cache_dir, merged);
    }

    return merged;
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


} // namespace lemon
