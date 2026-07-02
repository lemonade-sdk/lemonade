#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Server-side storage for task/application presets.
//
// This is intentionally separate from recipe_options:
// - recipe_options stay model/load/backend parameters.
// - presets are global user-facing profiles and may include behavior metadata
//   such as system prompts and tool defaults.
// - system prompts are stored as direct text, not as prompt IDs whose meaning can
//   drift when the frontend starter prompt catalog changes.
class PresetStore {
public:
    explicit PresetStore(std::string cache_dir);

    json list_presets() const;
    std::optional<json> get_preset(const std::string& id) const;
    json upsert_preset(json preset);
    bool delete_preset(const std::string& id);

    json export_store() const;
    json import_store(const json& payload);

    static std::string requested_preset_id(const json& request);
    static void merge_missing(json& target, const json& defaults);
    static std::string selected_system_prompt_text(const json& preset);

private:
    json load_user_store_unlocked() const;
    void save_user_store_unlocked(const json& store) const;

    static json empty_user_store();
    static json builtin_presets();
    static json sanitize_preset(json preset, bool force_starter);
    static bool is_builtin_id(const std::string& id);
    static bool is_valid_id(const std::string& id);

    std::string cache_dir_;
    mutable std::mutex mutex_;
};

} // namespace lemon
