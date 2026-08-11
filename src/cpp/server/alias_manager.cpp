#include "lemon/alias_manager.h"
#include "lemon/utils/model_name_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/aixlog.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {

AliasManager::AliasManager(std::string cache_dir)
    : cache_dir_(std::move(cache_dir)) {
    if (!cache_dir_.empty()) {
        load();
    }
}

void AliasManager::set_cache_dir(const std::string& cache_dir) {
    {
        std::unique_lock lock(mutex_);
        cache_dir_ = cache_dir;
    }
    if (!cache_dir.empty()) {
        load();
    } else {
        std::unique_lock lock(mutex_);
        aliases_.clear();
    }
}

std::string AliasManager::get_cache_dir() const {
    std::shared_lock lock(mutex_);
    return cache_dir_;
}

bool AliasManager::set_alias(const std::string& alias, const std::string& target, std::string& err_msg) {
    std::string clean_alias = utils::normalize_model_name(alias);
    std::string clean_target = utils::normalize_model_name(target);

    if (clean_alias.empty()) {
        err_msg = "Alias name cannot be empty";
        return false;
    }
    if (clean_target.empty()) {
        err_msg = "Alias target cannot be empty";
        return false;
    }
    if (clean_alias == clean_target) {
        err_msg = "Alias cannot point to itself";
        return false;
    }

    std::unordered_map<std::string, std::string> temp_aliases;
    {
        std::unique_lock lock(mutex_);
        temp_aliases = aliases_;
        temp_aliases[clean_alias] = clean_target;

        // Cycle detection check
        std::string current = clean_target;
        std::unordered_set<std::string> visited;
        visited.insert(clean_alias);

        size_t depth = 0;
        while (temp_aliases.count(current)) {
            if (visited.count(current)) {
                err_msg = "Circular alias dependency detected: " + clean_alias + " -> " + clean_target;
                LOG(WARNING, "AliasManager") << err_msg << std::endl;
                return false;
            }
            visited.insert(current);
            current = temp_aliases[current];
            depth++;
            if (depth > MAX_ALIAS_CHAIN_DEPTH) {
                err_msg = "Alias resolution chain exceeded max depth of " + std::to_string(MAX_ALIAS_CHAIN_DEPTH);
                return false;
            }
        }

        aliases_[clean_alias] = clean_target;
    }

    LOG(INFO, "AliasManager") << "Set alias: " << clean_alias << " -> " << clean_target << std::endl;
    save();
    return true;
}

bool AliasManager::remove_alias(const std::string& alias) {
    std::string clean_alias = utils::normalize_model_name(alias);
    bool removed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = aliases_.find(clean_alias);
        if (it != aliases_.end()) {
            aliases_.erase(it);
            removed = true;
        }
    }

    if (removed) {
        LOG(INFO, "AliasManager") << "Removed alias: " << clean_alias << std::endl;
        save();
    }
    return removed;
}

std::optional<std::string> AliasManager::resolve_alias(const std::string& alias_or_name) const {
    std::string current = utils::normalize_model_name(alias_or_name);
    std::shared_lock lock(mutex_);

    auto it = aliases_.find(current);
    if (it == aliases_.end()) {
        return std::nullopt;
    }

    std::unordered_set<std::string> visited;
    visited.insert(current);
    size_t depth = 0;

    while (it != aliases_.end() && depth < MAX_ALIAS_CHAIN_DEPTH) {
        std::string next_target = it->second;
        if (visited.count(next_target)) {
            LOG(WARNING, "AliasManager") << "Circular alias chain detected starting from '" << alias_or_name << "'" << std::endl;
            return std::nullopt;
        }
        current = next_target;
        visited.insert(current);
        depth++;
        it = aliases_.find(current);
    }

    if (depth >= MAX_ALIAS_CHAIN_DEPTH && it != aliases_.end()) {
        LOG(WARNING, "AliasManager") << "Alias chain depth exceeded " << MAX_ALIAS_CHAIN_DEPTH << " for '" << alias_or_name << "'" << std::endl;
        return std::nullopt;
    }

    return current;
}

bool AliasManager::has_alias(const std::string& alias) const {
    std::string clean_alias = utils::normalize_model_name(alias);
    std::shared_lock lock(mutex_);
    return aliases_.find(clean_alias) != aliases_.end();
}

std::unordered_map<std::string, std::string> AliasManager::get_all_aliases() const {
    std::shared_lock lock(mutex_);
    return aliases_;
}

void AliasManager::load() {
    std::string current_cache_dir;
    {
        std::shared_lock lock(mutex_);
        current_cache_dir = cache_dir_;
    }

    if (current_cache_dir.empty()) {
        return;
    }

    fs::path cache_path = utils::path_from_utf8(current_cache_dir);
    fs::path alias_path = cache_path / "aliases.json";

    if (!fs::exists(alias_path)) {
        std::unique_lock lock(mutex_);
        aliases_.clear();
        return;
    }

    std::unordered_map<std::string, std::string> loaded_aliases;
    bool parse_failed = false;

    {
        std::ifstream file(alias_path, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            LOG(WARNING, "AliasManager") << "Failed to open " << utils::path_to_utf8(alias_path) << std::endl;
            return;
        }

        try {
            json j = json::parse(file);
            if (j.is_object()) {
                for (auto& [k, v] : j.items()) {
                    if (v.is_string()) {
                        std::string norm_k = utils::normalize_model_name(k);
                        std::string norm_v = utils::normalize_model_name(v.get<std::string>());
                        if (!norm_k.empty() && !norm_v.empty() && norm_k != norm_v) {
                            loaded_aliases[norm_k] = norm_v;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            parse_failed = true;
            LOG(WARNING, "AliasManager") << "Failed to parse " << utils::path_to_utf8(alias_path)
                                         << ": " << e.what() << std::endl;
        }
    }

    if (parse_failed) {
        std::error_code ec;
        fs::path corrupted_path = cache_path / "aliases.json.corrupted";
        fs::rename(alias_path, corrupted_path, ec);
        LOG(WARNING, "AliasManager") << "Renamed corrupted aliases file to "
                                     << utils::path_to_utf8(corrupted_path) << std::endl;
        std::unique_lock lock(mutex_);
        aliases_.clear();
        return;
    }

    // Prune cyclic dependencies loaded from disk
    std::unordered_map<std::string, std::string> sanitized_aliases;
    for (const auto& [alias_k, target_v] : loaded_aliases) {
        std::unordered_set<std::string> visited;
        visited.insert(alias_k);
        std::string curr = target_v;
        bool is_cyclic = false;
        size_t depth = 0;

        while (loaded_aliases.count(curr) && depth < MAX_ALIAS_CHAIN_DEPTH) {
            if (visited.count(curr)) {
                is_cyclic = true;
                break;
            }
            visited.insert(curr);
            curr = loaded_aliases.at(curr);
            depth++;
        }

        if (visited.count(curr) || depth >= MAX_ALIAS_CHAIN_DEPTH) {
            is_cyclic = true;
        }

        if (is_cyclic) {
            LOG(WARNING, "AliasManager") << "Pruning cyclic alias entry from disk: " << alias_k << " -> " << target_v << std::endl;
        } else {
            sanitized_aliases[alias_k] = target_v;
        }
    }

    {
        std::unique_lock lock(mutex_);
        aliases_ = std::move(sanitized_aliases);
    }
    LOG(INFO, "AliasManager") << "Loaded " << aliases_.size() << " alias binding(s) from disk." << std::endl;
}

void AliasManager::save() {
    std::string current_cache_dir;
    std::unordered_map<std::string, std::string> aliases_copy;
    {
        std::shared_lock lock(mutex_);
        current_cache_dir = cache_dir_;
        aliases_copy = aliases_;
    }

    if (current_cache_dir.empty()) {
        return;
    }

    fs::path cache_path = utils::path_from_utf8(current_cache_dir);
    if (!fs::exists(cache_path)) {
        std::error_code ec;
        fs::create_directories(cache_path, ec);
    }

    fs::path target_path = cache_path / "aliases.json";
    fs::path temp_path = cache_path / "aliases.json.tmp";

    // Scope output file stream so handle closes before rename/copy
    {
        std::ofstream file(temp_path, std::ios::out | std::ios::binary);
        if (!file.is_open()) {
            LOG(ERROR, "AliasManager") << "Failed to open temp file for writing: "
                                       << utils::path_to_utf8(temp_path) << std::endl;
            return;
        }

        json j = json::object();
        for (const auto& [k, v] : aliases_copy) {
            j[k] = v;
        }
        file << j.dump(2) << "\n";
    }

#ifndef _WIN32
    chmod(temp_path.c_str(), 0600);
#endif

    std::error_code ec;
    fs::rename(temp_path, target_path, ec);
    if (ec) {
        std::error_code copy_ec;
        fs::copy_file(temp_path, target_path, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            LOG(ERROR, "AliasManager") << "Failed to replace " << utils::path_to_utf8(target_path)
                                       << ": " << copy_ec.message() << std::endl;
        } else {
#ifndef _WIN32
            chmod(target_path.c_str(), 0600);
#endif
        }
        fs::remove(temp_path, ec);
    }
}

} // namespace lemon
