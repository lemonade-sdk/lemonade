#include <lemon/models/recipe_options_store.h>
#include <lemon/canonical_id.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/json_file.h>

#include <utility>

namespace lemon {

RecipeOptionsStore::RecipeOptionsStore(std::string file_path)
    : file_path_(std::move(file_path)) {
    options_ = utils::load_optional_json(file_path_);
    if (!options_.is_object()) {
        if (!options_.is_null()) {
            LOG(WARNING, "ModelManager")
                << "recipe_options.json is not a JSON object; resetting to empty object"
                << std::endl;
        }
        options_ = json::object();
    }
}

void RecipeOptionsStore::migrate(
    const std::function<bool(const std::string& bare_name)>& known_builtin) {
    int moved = 0;
    json normalized = json::object();
    for (auto it = options_.begin(); it != options_.end(); ++it) {
        const std::string& key = it.key();
        if (parse_canonical_id(key)) {
            normalized[key] = it.value();
        } else if (known_builtin(key)) {
            normalized[canonical_id(ModelSource::Builtin, key)] = it.value();
            ++moved;
        } else {
            normalized[key] = it.value();
        }
    }
    if (moved == 0) return;

    options_ = std::move(normalized);
    try {
        utils::save_json_atomic(file_path_, options_);
        LOG(INFO, "ModelManager")
            << "migrated " << moved << " legacy recipe_options keys to builtin. prefix"
            << std::endl;
    } catch (const std::exception& e) {
        // A read-only config dir must not break startup: the in-memory set is
        // already normalized, so the next write persists it anyway.
        LOG(WARNING, "ModelManager")
            << "Could not persist migrated recipe_options.json: " << e.what() << std::endl;
    }
}

json RecipeOptionsStore::entry(const std::string& canonical_id_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_locked(canonical_id_value);
}

json RecipeOptionsStore::entry_locked(const std::string& canonical_id_value) const {
    if (options_.contains(canonical_id_value) && options_[canonical_id_value].is_object()) {
        return options_[canonical_id_value];
    }
    return json::object();
}

json RecipeOptionsStore::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

json RecipeOptionsStore::replace(const std::string& canonical_id_value, const json& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    store(canonical_id_value, value.is_object() ? value : json::object());
    return entry_locked(canonical_id_value);
}

json RecipeOptionsStore::merge(const std::string& canonical_id_value, const json& changes) {
    std::lock_guard<std::mutex> lock(mutex_);
    json merged = json::object();
    if (options_.contains(canonical_id_value) && options_[canonical_id_value].is_object()) {
        merged = options_[canonical_id_value];
    }
    if (changes.is_object()) {
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            if (it.value().is_null()) {
                merged.erase(it.key());
            } else {
                merged[it.key()] = it.value();
            }
        }
    }
    store(canonical_id_value, merged);
    return entry_locked(canonical_id_value);
}

void RecipeOptionsStore::store(const std::string& canonical_id_value, const json& value) {
    if (value.empty()) {
        options_.erase(canonical_id_value);
    } else {
        options_[canonical_id_value] = value;
    }
    utils::save_json_atomic(file_path_, options_);
}

}  // namespace lemon
