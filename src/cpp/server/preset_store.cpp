#include "lemon/preset_store.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {

namespace {

fs::path store_path_for(const std::string& cache_dir) {
    return utils::path_from_utf8(cache_dir) / "presets.json";
}

std::string string_or(const json& object, const std::string& key, const std::string& fallback = "") {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return fallback;
    }
    return object[key].get<std::string>();
}

json object_or_empty(const json& value) {
    return value.is_object() ? value : json::object();
}

json array_or_empty(const json& value) {
    return value.is_array() ? value : json::array();
}

json make_system_prompt(const std::string& id, const std::string& name, const std::string& text, bool built_in) {
    if (text.empty()) {
        return json::object();
    }
    return {
        {"id", id},
        {"name", name},
        {"text", text},
        {"built_in", built_in},
        {"mode", "prepend"}
    };
}

json make_system_prompts_array(const std::string& id, const std::string& name, const std::string& text, bool built_in) {
    if (text.empty()) {
        return json::array();
    }
    return json::array({{
        {"id", id},
        {"name", name},
        {"prompt", text},
        {"built_in", built_in}
    }});
}

json make_preset(const std::string& id,
                 const std::string& name,
                 const std::string& description,
                 json applies_to,
                 json recipe_options,
                 json sampling,
                 const std::string& engine_hint,
                 const std::string& prompt_text,
                 bool tools_enabled) {
    return {
        {"id", id},
        {"name", name},
        {"description", description},
        {"applies_to", std::move(applies_to)},
        {"recipe_options", std::move(recipe_options)},
        {"sampling", std::move(sampling)},
        {"engine_hint", engine_hint},
        {"starter", true},
        {"locked", true},
        {"tools_enabled", tools_enabled},
        {"system_prompt_id", prompt_text.empty() ? "none" : "general"},
        {"system_prompt", make_system_prompt("general", "General", prompt_text, true)},
        {"system_prompts", make_system_prompts_array("general", "General", prompt_text, true)}
    };
}

void throw_invalid_preset(const std::string& message) {
    throw std::invalid_argument("Invalid preset: " + message);
}

bool has_nonempty_string(const json& object, const std::string& key) {
    return object.is_object() && object.contains(key) && object[key].is_string() && !object[key].get<std::string>().empty();
}

bool bool_or(const json& object, const std::string& key, bool fallback) {
    if (!object.is_object() || !object.contains(key)) {
        return fallback;
    }
    if (!object[key].is_boolean()) {
        return fallback;
    }
    return object[key].get<bool>();
}

} // namespace

PresetStore::PresetStore(std::string cache_dir) : cache_dir_(std::move(cache_dir)) {}

json PresetStore::empty_user_store() {
    return {
        {"schema_version", 1},
        {"presets", json::array()}
    };
}

bool PresetStore::is_valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
    });
}

bool PresetStore::is_builtin_id(const std::string& id) {
    for (const auto& preset : builtin_presets()) {
        if (preset.value("id", "") == id) {
            return true;
        }
    }
    return false;
}

json PresetStore::builtin_presets() {
    static const json presets = json::array({
        make_preset(
            "s-default", "Default",
            "Use current model defaults and automatic backend selection.",
            json::array({"all"}), json::object(), json::object(),
            "auto", "", true),
        make_preset(
            "s-balanced", "Balanced",
            "Sensible defaults. Good first pick for everyday chat.",
            json::array({"chat"}), {{"ctx_size", 16384}},
            {{"temperature", 0.70}, {"top_p", 0.90}, {"top_k", 40}, {"repeat_penalty", 1.05}},
            "llamacpp",
            "Answer directly and practically. Keep responses concise unless the task needs detail, and avoid unnecessary setup.",
            true),
        make_preset(
            "s-thorough", "Thorough",
            "Careful answers for analysis, planning, debugging, and decisions.",
            json::array({"chat"}), {{"ctx_size", 32768}},
            {{"temperature", 0.40}, {"top_p", 0.95}, {"top_k", 40}, {"repeat_penalty", 1.10}},
            "llamacpp",
            "Work carefully and systematically. Give the reasoning outcome, tradeoffs, and recommended next step without exposing private chain-of-thought.",
            true),
        make_preset(
            "s-quick-chat", "Quick Chat",
            "Small context, tight sampling. Snappy responses for quick interactions.",
            json::array({"chat"}), {{"ctx_size", 4096}},
            {{"temperature", 0.60}, {"top_p", 0.80}, {"top_k", 40}, {"repeat_penalty", 1.05}},
            "llamacpp",
            "Be brief and responsive. Answer the user's direct request with minimal setup.",
            false),
        make_preset(
            "s-creative", "Creative",
            "Higher temperature for brainstorming, dialog, and divergent thinking.",
            json::array({"chat"}), {{"ctx_size", 32768}},
            {{"temperature", 0.95}, {"top_p", 0.95}, {"top_k", 60}, {"repeat_penalty", 1.00}},
            "llamacpp",
            "Help the user explore original ideas while staying useful and grounded. Offer variations, not just one answer.",
            true),
        make_preset(
            "s-long-context", "Long Context",
            "For documents, codebases, and long conversation threads.",
            json::array({"chat"}), {{"ctx_size", 262144}},
            {{"temperature", 0.70}, {"top_p", 0.90}, {"top_k", 40}, {"repeat_penalty", 1.05}},
            "llamacpp",
            "Handle long inputs by extracting the user's goal, preserving important details, and avoiding unnecessary repetition.",
            true),
        make_preset(
            "s-code", "Code",
            "Low temperature, tight sampling for code generation and refactoring.",
            json::array({"chat", "code"}), {{"ctx_size", 131072}},
            {{"temperature", 0.20}, {"top_p", 0.95}, {"top_k", 40}, {"repeat_penalty", 1.05}},
            "llamacpp",
            "Act as a careful coding assistant. Prefer small, maintainable changes, explain the impact, preserve existing style, and keep compatibility in mind.",
            true),
        make_preset(
            "s-quality", "Quality",
            "More steps and tighter guidance for crisp, deliberate image generation.",
            json::array({"image"}), {{"steps", 20}, {"cfg_scale", 8.0}}, json::object(),
            "sd-cpp",
            "Help produce high-quality image results. Clarify subject, composition, style, lighting, constraints, and negative details only when useful.",
            false),
        make_preset(
            "s-preview", "Preview",
            "Fewer steps, looser guidance - fast drafts and iteration.",
            json::array({"image"}), {{"steps", 8}, {"cfg_scale", 6.0}}, json::object(),
            "sd-cpp",
            "Optimize for fast image drafts. Keep prompts compact, focus on the main subject, and make iteration easy.",
            false),
        make_preset(
            "s-turbo", "Turbo",
            "Fastest image drafts for rapid iteration.",
            json::array({"image"}), {{"steps", 4}, {"cfg_scale", 1.0}}, json::object(),
            "sd-cpp",
            "Optimize for the fastest usable image result. Keep instructions minimal and avoid over-specification.",
            false)
    });
    return presets;
}

json PresetStore::sanitize_preset(json preset, bool force_starter) {
    if (!preset.is_object()) {
        throw_invalid_preset("preset must be a JSON object");
    }

    const std::string id = string_or(preset, "id");
    if (!is_valid_id(id)) {
        throw_invalid_preset("id must be non-empty and contain only letters, numbers, '.', '_', '-' or ':'");
    }

    if (!has_nonempty_string(preset, "name")) {
        throw_invalid_preset("name must be a non-empty string");
    }

    json sanitized = preset;
    sanitized["id"] = id;
    sanitized["name"] = string_or(preset, "name", "Untitled");
    sanitized["description"] = string_or(preset, "description");

    json applies_to = array_or_empty(preset.value("applies_to", json::array()));
    if (applies_to.empty()) {
        applies_to = json::array({id == "s-default" ? "all" : "chat"});
    }
    sanitized["applies_to"] = applies_to;
    sanitized["recipe_options"] = object_or_empty(preset.value("recipe_options", json::object()));
    sanitized["sampling"] = object_or_empty(preset.value("sampling", json::object()));
    sanitized["engine_hint"] = string_or(preset, "engine_hint", "auto");
    sanitized["starter"] = force_starter || bool_or(preset, "starter", false);
    sanitized["tools_enabled"] = bool_or(preset, "tools_enabled", true);

    if (!sanitized.contains("system_prompt_id") || !sanitized["system_prompt_id"].is_string()) {
        sanitized["system_prompt_id"] = selected_system_prompt_text(sanitized).empty() ? "none" : "general";
    }

    if (sanitized.contains("system_prompts")) {
        json cleaned = json::array();
        std::set<std::string> seen;
        for (const auto& item : array_or_empty(sanitized["system_prompts"])) {
            if (!item.is_object()) continue;
            const std::string prompt_id = string_or(item, "id");
            const std::string prompt_name = string_or(item, "name");
            const std::string prompt_text = string_or(item, "prompt");
            if (!is_valid_id(prompt_id) || prompt_name.empty() || prompt_text.empty() || seen.count(prompt_id) > 0) {
                continue;
            }
            seen.insert(prompt_id);
            cleaned.push_back({
                {"id", prompt_id},
                {"name", prompt_name},
                {"prompt", prompt_text},
                {"built_in", bool_or(item, "built_in", false)}
            });
        }
        sanitized["system_prompts"] = cleaned;
    }

    const std::string prompt_text = selected_system_prompt_text(sanitized);
    if (!prompt_text.empty()) {
        json current = object_or_empty(sanitized.value("system_prompt", json::object()));
        // Keep the stable direct-text field in sync with the selected prompt.
        // This prevents a stale system_prompt.text from overriding a changed
        // system_prompt_id/system_prompts selection after import or UI edits.
        current["text"] = prompt_text;
        if (!current.contains("mode") || !current["mode"].is_string()) {
            current["mode"] = "prepend";
        }
        sanitized["system_prompt"] = current;
    } else {
        sanitized.erase("system_prompt");
    }

    return sanitized;
}

json PresetStore::load_user_store_unlocked() const {
    if (cache_dir_.empty()) {
        return empty_user_store();
    }

    const fs::path path = store_path_for(cache_dir_);
    if (!fs::exists(path)) {
        return empty_user_store();
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open " + utils::path_to_utf8(path));
    }

    json loaded;
    try {
        loaded = json::parse(file);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse " + utils::path_to_utf8(path) + ": " + e.what());
    }

    json store = empty_user_store();
    if (loaded.contains("schema_version") && loaded["schema_version"].is_number_integer()) {
        store["schema_version"] = loaded["schema_version"];
    }
    if (loaded.contains("presets") && loaded["presets"].is_array()) {
        store["presets"] = loaded["presets"];
    }
    return store;
}

void PresetStore::save_user_store_unlocked(const json& store) const {
    if (cache_dir_.empty()) {
        throw std::runtime_error("Preset storage is unavailable because cache_dir is empty");
    }

    const fs::path cache_path = utils::path_from_utf8(cache_dir_);
    if (!fs::exists(cache_path)) {
        fs::create_directories(cache_path);
    }

    const fs::path path = store_path_for(cache_dir_);
    fs::path temp_path = path;
    temp_path += ".tmp";

    {
        std::ofstream file(temp_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to write " + utils::path_to_utf8(temp_path));
        }
        file << store.dump(2) << std::endl;
    }

    std::error_code ec;
    fs::rename(temp_path, path, ec);
    if (ec) {
        std::error_code copy_ec;
        fs::copy_file(temp_path, path, fs::copy_options::overwrite_existing, copy_ec);
        fs::remove(temp_path);
        if (copy_ec) {
            throw std::runtime_error("Failed to save " + utils::path_to_utf8(path) + ": " + copy_ec.message());
        }
    }
}

json PresetStore::list_presets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json result = builtin_presets();
    json store = load_user_store_unlocked();

    std::set<std::string> seen;
    for (const auto& preset : result) {
        seen.insert(preset.value("id", ""));
    }

    for (const auto& raw : array_or_empty(store.value("presets", json::array()))) {
        try {
            json preset = sanitize_preset(raw, false);
            const std::string id = preset.value("id", "");
            if (!seen.count(id)) {
                result.push_back(std::move(preset));
                seen.insert(id);
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "PresetStore") << "Skipping invalid stored preset: " << e.what() << std::endl;
        }
    }

    return result;
}

std::optional<json> PresetStore::get_preset(const std::string& id) const {
    for (const auto& preset : list_presets()) {
        if (preset.value("id", "") == id) {
            return preset;
        }
    }
    return std::nullopt;
}

json PresetStore::upsert_preset(json preset) {
    json sanitized = sanitize_preset(std::move(preset), false);
    const std::string id = sanitized.value("id", "");
    if (is_builtin_id(id)) {
        throw std::invalid_argument("Built-in preset '" + id + "' cannot be overwritten; clone it to a custom id instead");
    }
    sanitized["starter"] = false;
    sanitized["locked"] = false;

    std::lock_guard<std::mutex> lock(mutex_);
    json store = load_user_store_unlocked();
    json& presets = store["presets"];
    bool replaced = false;
    for (auto& item : presets) {
        if (item.is_object() && item.value("id", "") == id) {
            item = sanitized;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        presets.push_back(sanitized);
    }
    save_user_store_unlocked(store);
    return sanitized;
}

bool PresetStore::delete_preset(const std::string& id) {
    if (is_builtin_id(id)) {
        throw std::invalid_argument("Built-in preset '" + id + "' cannot be deleted");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    json store = load_user_store_unlocked();
    json& presets = store["presets"];
    const size_t before = presets.size();
    presets.erase(std::remove_if(presets.begin(), presets.end(), [&](const json& item) {
        return item.is_object() && item.value("id", "") == id;
    }), presets.end());

    if (presets.size() != before) {
        save_user_store_unlocked(store);
        return true;
    }
    return false;
}

json PresetStore::export_store() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json store = load_user_store_unlocked();
    store["schema_version"] = 1;
    store["builtin_presets"] = builtin_presets();
    return store;
}

json PresetStore::import_store(const json& payload) {
    json incoming = payload;
    if (incoming.contains("presets")) {
        // already a store-like object
    } else if (incoming.is_array()) {
        incoming = {{"schema_version", 1}, {"presets", incoming}};
    } else {
        throw std::invalid_argument("Import body must be a preset array or an object with presets");
    }

    json next = empty_user_store();
    std::set<std::string> seen;
    for (const auto& raw : array_or_empty(incoming.value("presets", json::array()))) {
        json preset = sanitize_preset(raw, false);
        const std::string id = preset.value("id", "");
        if (is_builtin_id(id) || seen.count(id)) {
            continue;
        }
        preset["starter"] = false;
        preset["locked"] = false;
        next["presets"].push_back(std::move(preset));
        seen.insert(id);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    save_user_store_unlocked(next);
    return next;
}

std::string PresetStore::requested_preset_id(const json& request) {
    for (const std::string key : {"preset", "preset_id"}) {
        if (request.contains(key) && request[key].is_string()) {
            const std::string value = request[key].get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
    }
    return "";
}

void PresetStore::merge_missing(json& target, const json& defaults) {
    if (!target.is_object() || !defaults.is_object()) {
        return;
    }
    for (const auto& [key, value] : defaults.items()) {
        if (!target.contains(key)) {
            target[key] = value;
        }
    }
}

std::string PresetStore::selected_system_prompt_text(const json& preset) {
    if (!preset.is_object()) {
        return "";
    }

    // Prototype/UI compatibility: when a selected prompt id is present, honor the
    // selected entry first. sanitize_preset() then mirrors that text into the
    // stable system_prompt.text field for long-term storage.
    const std::string prompt_id = string_or(preset, "system_prompt_id", "none");
    if (prompt_id != "none" && !prompt_id.empty()) {
        for (const auto& prompt : array_or_empty(preset.value("system_prompts", json::array()))) {
            if (!prompt.is_object() || prompt.value("id", "") != prompt_id) continue;
            if (prompt.contains("prompt") && prompt["prompt"].is_string()) {
                return prompt["prompt"].get<std::string>();
            }
            if (prompt.contains("text") && prompt["text"].is_string()) {
                return prompt["text"].get<std::string>();
            }
        }
    }

    // Stable canonical form for server-side presets and imports.
    if (preset.contains("system_prompt")) {
        const auto& value = preset["system_prompt"];
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_object()) {
            if (value.contains("text") && value["text"].is_string()) {
                return value["text"].get<std::string>();
            }
            if (value.contains("content") && value["content"].is_string()) {
                return value["content"].get<std::string>();
            }
            if (value.contains("prompt") && value["prompt"].is_string()) {
                return value["prompt"].get<std::string>();
            }
        }
    }

    return "";
}

} // namespace lemon
