#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <string>

namespace lemon {

using json = nlohmann::json;

// Persistence for recipe_options.json, keyed by canonical model id.
class RecipeOptionsStore {
public:
    // A missing, unparseable or non-object file reads as an empty set.
    explicit RecipeOptionsStore(std::string file_path);

// Renames keys written before ids were canonical; unrecognized keys are kept.
    void migrate(const std::function<bool(const std::string& bare_name)>& known_builtin);

    // What the user saved for one model, or an empty object.
    json entry(const std::string& canonical_id) const;

    // The whole map, for a caller folding layers across many models in one pass.
    json all() const;

// Replaces the entry outright; an empty value removes it.
    json replace(const std::string& canonical_id, const json& value);

// Applies changes over the stored entry; a null value removes that option.
    json merge(const std::string& canonical_id, const json& changes);

    // Full durable state, including inactive recipes, for registration rollback.
    json snapshot() const;
    void restore_snapshot(const json& snapshot);

    // Switch only the active saved settings. Inactive recipe profiles remain
    // server-owned, keyed by the same canonical model id.
    void switch_recipe(const std::string& canonical_id, const std::string& previous_recipe,
                       const std::string& next_recipe, const json& previous_saved,
                       const json& next_authored = json::object());

    // Called when a custom definition is removed, not when a recipe changes.
    void forget_model(const std::string& canonical_id);

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
