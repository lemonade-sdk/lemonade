#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <string>

namespace lemon {

using json = nlohmann::json;

// Persistence and CRUD for recipe_options.json, keyed by canonical model ID.
// The precedence ladder that folds these over registry defaults is not here.
class RecipeOptionsStore {
public:
    // A missing, unparseable or non-object file reads as an empty set.
    explicit RecipeOptionsStore(std::string file_path);

    // Renames keys written before IDs were canonical. Anything unrecognized is
    // preserved so a stale entry keeps its options. Logs how many keys moved.
    void migrate(const std::function<bool(const std::string& bare_name)>& known_builtin);

    // What the user saved for one model, or an empty object.
    json entry(const std::string& canonical_id) const;

    // The whole map, for a caller folding layers across many models in one pass.
    json all() const;

    // Stores one model's entry, replacing it outright. An empty value removes the
    // row. Returns what is now stored.
    json replace(const std::string& canonical_id, const json& value);

    // Applies changes over the stored entry, where a null value removes that
    // option. A row left empty is removed. Returns what is now stored.
    json merge(const std::string& canonical_id, const json& changes);

    const std::string& file_path() const { return file_path_; }

private:
    // Callers hold mutex_.
    void store(const std::string& canonical_id, const json& value);
    json entry_locked(const std::string& canonical_id) const;

    std::string file_path_;
    mutable std::mutex mutex_;
    json options_ = json::object();
};

}  // namespace lemon
