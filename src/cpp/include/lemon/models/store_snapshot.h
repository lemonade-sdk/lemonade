#pragma once

#include <lemon/model_manager.h>
#include <lemon/models/store_builder.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

// One immutable view of the model set. Readers hold a shared_ptr, so anything
// they read out of a snapshot stays valid across the next publish.
struct StoreSnapshot {
    // Canonical id -> info.
    std::map<std::string, ModelInfo> models;
    // Any accepted public name -> canonical id.
    std::map<std::string, std::string> public_model_aliases;
    // Canonical id -> the name it is listed under.
    std::map<std::string, std::string> canonical_public_names;
    std::shared_ptr<const FilterReasons> reasons;
    // False until the first build; readers rebuild when they see it.
    bool valid = false;
    std::uint64_t generation = 0;

    // Pointer into this snapshot, so it lives as long as the snapshot does.
    const ModelInfo* find(const std::string& canonical_id) const {
        auto it = models.find(canonical_id);
        return it != models.end() ? &it->second : nullptr;
    }

    std::string canonical_for(const std::string& public_name) const {
        auto it = public_model_aliases.find(public_name);
        return it != public_model_aliases.end() ? it->second : std::string();
    }

    std::string public_name_for(const std::string& canonical_id) const {
        auto it = canonical_public_names.find(canonical_id);
        return it != canonical_public_names.end() ? it->second : std::string();
    }

    std::string rejection_reason(const std::string& canonical_id) const {
        if (!reasons) {
            return std::string();
        }
        for (const auto& rejected : reasons->rejected) {
            if (rejected.canonical_id == canonical_id) {
                return rejected.reason;
            }
        }
        return std::string();
    }

    std::vector<std::string> empty_recipes() const {
        if (!reasons) {
            return {};
        }
        return reasons->empty_recipes;
    }
};

}  // namespace models
}  // namespace lemon
