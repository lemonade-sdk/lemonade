#include "lemon_cli/tui_launch_model.h"

#include <lemon/canonical_id.h>
#include <lemon/model_types.h>
#include <lemon/utils/url_utils.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace lemon_cli {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string string_field(const nlohmann::json& value, const char* key, const char* fallback = "") {
    const auto field = value.find(key);
    return field != value.end() && field->is_string() ? field->get<std::string>() : fallback;
}

bool has_label(const LaunchModel& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

bool is_launch_model(const LaunchModel& model) {
    return (model.recipe == "llamacpp" || model.recipe == "flm" || model.recipe == "vllm") &&
           lemon::get_model_type_from_labels(model.labels) == lemon::ModelType::LLM;
}

} // namespace

bool launch_uses_tui(bool terminal, std::optional<bool> override,
                     const std::string& agent, const std::string& model) {
    return terminal && override.value_or(agent.empty() || model.empty());
}

std::vector<LaunchModel> parse_launch_models(const nlohmann::json& response,
                                           const nlohmann::json& health) {
    if (!response.is_object() || !response.contains("data") || !response["data"].is_array()) {
        throw std::runtime_error("Unexpected model listing response");
    }
    std::vector<LaunchModel> models;
    for (const auto& item : response["data"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        LaunchModel model;
        model.id = item["id"].get<std::string>();
        model.recipe = item.value("recipe", "");
        model.checkpoint = item.value("checkpoint", "");
        model.registry_source = lower(item.value("source", item.value("registry_source", "")));
        model.downloaded = item.value("downloaded", false);
        model.storage_known = item.contains("downloaded") && item["downloaded"].is_boolean();
        if (item.contains("checkpoints") && item["checkpoints"].is_object()) model.checkpoints = item["checkpoints"];
        else if (!model.checkpoint.empty()) model.checkpoints = {{"main", model.checkpoint}};
        if (model.checkpoint.empty() && model.checkpoints.contains("main") && model.checkpoints["main"].is_string()) {
            model.checkpoint = model.checkpoints["main"].get<std::string>();
        }
        if (item.contains("labels") && item["labels"].is_array()) {
            for (const auto& label : item["labels"]) {
                if (label.is_string()) model.labels.push_back(label.get<std::string>());
            }
        }
        if (item.contains("size") && item["size"].is_number() && item["size"].get<double>() > 0) {
            model.size_gb = item["size"].get<double>();
        }
        if (item.contains("recipe_options") && item["recipe_options"].is_object()) {
            model.recipe_options = item["recipe_options"];
        }
        if (health.contains("all_models_loaded") && health["all_models_loaded"].is_array()) {
            model.runtime = "unloaded";
            for (const auto& loaded : health["all_models_loaded"]) {
                if (loaded.is_object() && string_field(loaded, "model_name") == model.id) {
                    model.runtime = !model.recipe.empty() && !model.checkpoint.empty() &&
                        string_field(loaded, "checkpoint") == model.checkpoint && string_field(loaded, "recipe") == model.recipe ?
                        string_field(loaded, "status", "unknown") : "unknown";
                }
            }
        }
        if (!model.id.empty()) models.push_back(std::move(model));
    }
    return models;
}

std::string launch_preset_url(const std::string& repo_dir, const std::string& recipe_file) {
    std::string url = "https://raw.githubusercontent.com/lemonade-sdk/recipes/main";
    size_t start = 0;
    while (start <= repo_dir.size()) {
        const size_t end = std::min(repo_dir.find('/', start), repo_dir.size());
        if (end > start) url += "/" + lemon::utils::url_encode(repo_dir.substr(start, end - start));
        start = end + 1;
    }
    return url + "/" + lemon::utils::url_encode(recipe_file);
}

LaunchModel parse_launch_preset(const nlohmann::json& payload) {
    if (!payload.is_object()) throw std::runtime_error("Preset must be a JSON object");
    const bool has_checkpoints = payload.contains("checkpoints") && payload["checkpoints"].is_object();
    const bool has_checkpoint = payload.contains("checkpoint") && payload["checkpoint"].is_string();
    if (!has_checkpoints && !has_checkpoint) throw std::runtime_error("Preset must define model checkpoints");
    if (payload.contains("recipe_options") && !payload["recipe_options"].is_object()) {
        throw std::runtime_error("Preset recipe_options must be an object");
    }
    auto listing = payload;
    std::string name = payload.value("model_name", payload.value("id", ""));
    if (name.empty() || lemon::is_reserved_registration_name(name)) {
        throw std::runtime_error("Preset must register a non-empty user model name");
    }
    const auto canonical = lemon::parse_canonical_id(name);
    if (canonical && canonical->bare_name.empty()) throw std::runtime_error("Preset model name is empty");
    if (!canonical) name = "user." + name;
    listing["id"] = name;
    const auto models = parse_launch_models({{"data", nlohmann::json::array({listing})}}, nlohmann::json::object());
    if (models.empty() || !is_launch_model(models.front())) {
        throw std::runtime_error("Preset does not define a supported chat model");
    }
    auto model = models.front();
    model.downloaded = false;
    model.storage_known = false;
    return model;
}

LaunchModel reconcile_launch_preset(LaunchModel preset, const std::vector<LaunchModel>& models) {
    preset.downloaded = false;
    preset.storage_known = false;
    preset.runtime = "unknown";
    for (const auto& model : models) {
        if ((model.id == preset.id || model.id == lemon::strip_user_model_prefix(preset.id)) &&
            model.recipe == preset.recipe && !preset.checkpoints.empty() && model.checkpoints == preset.checkpoints &&
            !preset.registry_source.empty() && model.registry_source == preset.registry_source) {
            preset.downloaded = model.downloaded;
            preset.storage_known = model.storage_known;
            preset.runtime = model.runtime;
            break;
        }
    }
    return preset;
}

std::string launch_model_warning(const LaunchModel& model, const std::string& agent) {
    if (agent == "codex" && lower(model.id).find("qwen3.5") != std::string::npos) {
        return "Qwen3.5 models are currently avoided for Codex. Choose another model if possible.";
    }
    if (!has_label(model, "tool-calling")) {
        return "Tool-calling support is not declared for this model; agent workflows may fail.";
    }
    return {};
}

std::vector<int> filter_launch_models(const std::vector<LaunchModel>& models,
                                      LaunchTab tab, const std::string& query,
                                      const std::string& agent) {
    std::vector<int> result;
    const auto needle = lower(query);
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        if (!is_launch_model(model) || tab == LaunchTab::Presets) continue;
        if (tab == LaunchTab::Downloaded && !model.downloaded) continue;
        if (tab == LaunchTab::Recommended &&
            (!has_label(model, "hot") || !launch_model_warning(model, agent).empty())) continue;
        std::string searchable = model.id + " " + model.checkpoint + " " + model.recipe;
        for (const auto& label : model.labels) searchable += " " + label;
        if (!needle.empty() && lower(searchable).find(needle) == std::string::npos) continue;
        result.push_back(static_cast<int>(i));
    }
    std::sort(result.begin(), result.end(), [&](int lhs, int rhs) {
        if (models[lhs].downloaded != models[rhs].downloaded) return models[lhs].downloaded;
        return models[lhs].id < models[rhs].id;
    });
    return result;
}

bool apply_launch_context(const std::string& value, nlohmann::json& options,
                          std::string& error) {
    error.clear();
    const auto normalized = lower(value);
    if (normalized.empty() || normalized == "inherit") {
        if (!options.is_object()) options = nlohmann::json::object();
        options.erase("ctx_size");
        return true;
    }
    if (normalized == "auto" || normalized == "-1") {
        options["ctx_size"] = -1;
        return true;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        error = "Context must be inherit, auto, or a positive integer.";
        return false;
    }
    try {
        const auto parsed = std::stoll(value);
        if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) throw std::out_of_range("context");
        options["ctx_size"] = static_cast<int>(parsed);
        return true;
    } catch (const std::exception&) {
        error = "Context must be a positive integer within the supported range.";
        return false;
    }
}

} // namespace lemon_cli
