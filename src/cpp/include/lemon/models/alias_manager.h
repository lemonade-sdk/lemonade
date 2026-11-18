#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>

namespace lemon {
namespace models {

class AliasManager {
public:
    static constexpr size_t MAX_ALIAS_CHAIN_DEPTH = 10;

    explicit AliasManager(std::string cache_dir = "");

    void set_cache_dir(const std::string& cache_dir);
    std::string get_cache_dir() const;

    /**
     * Set or update an alias binding: alias -> target.
     * Returns true on success. Returns false and sets err_msg on validation or cycle error.
     */
    bool set_alias(const std::string& alias, const std::string& target, std::string& err_msg);

    /**
     * Remove an alias binding. Returns true if the alias was present and removed.
     */
    bool remove_alias(const std::string& alias);

    /**
     * Resolve a model name through the alias map (supporting multi-hop chaining up to MAX_ALIAS_CHAIN_DEPTH).
     * If the name is not an alias, returns std::nullopt.
     */
    std::optional<std::string> resolve_alias(const std::string& alias_or_name) const;

    /**
     * Check if a string is registered as an alias key.
     */
    bool has_alias(const std::string& alias) const;

    /**
     * Get a copy of all current alias bindings (alias -> target).
     */
    std::map<std::string, std::string> get_all_aliases() const;

    /**
     * Load aliases from cache_dir / "aliases.json".
     */
    void load();

    /**
     * Save current aliases to cache_dir / "aliases.json".
     *
     * Takes the exclusive lock and keeps it through the rename. Callers hold no lock.
     */
    void save();

private:
    std::string cache_dir_;
    // Also orders saves of aliases.json: a save holds it through the rename.
    mutable std::shared_mutex mutex_;
    std::map<std::string, std::string> aliases_;
    uint64_t save_sequence_ = 0;
};

}  // namespace models
}  // namespace lemon
