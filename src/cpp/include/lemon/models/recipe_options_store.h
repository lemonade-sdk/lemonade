#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <mutex>
#include <string>

namespace lemon {

using json = nlohmann::json;

// Persistence and CRUD for recipe_options.json, keyed by canonical model ID.
//
// Only persistence lives here. The precedence ladder that folds these entries
// over registry defaults and architecture overlays belongs to whoever holds the
// catalogs, so this store reads no catalog and no artifact state, and reaches no
// other component.
class RecipeOptionsStore {
public:
    // Reads the file now. A missing or unparseable file is an empty set; so is a
    // file whose top level is not an object, which older releases could produce.
    explicit RecipeOptionsStore(std::string file_path);

    // Renames keys written before IDs were canonical: a bare name that
    // known_builtin recognises gains the builtin. prefix, an already canonical
    // key is kept as is, and anything else is preserved so a stale entry does not
    // silently lose its options. Logs how many keys it moved.
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
