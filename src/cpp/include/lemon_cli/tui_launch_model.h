#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lemon_cli {

enum class LaunchTab { Downloaded, Recommended, Presets, All };

struct LaunchModel {
    std::string id;
    std::string recipe;
    std::string checkpoint;
    std::string registry_source;
    std::vector<std::string> labels;
    bool downloaded = false;
    bool storage_known = false;
    nlohmann::json checkpoints = nlohmann::json::object();
    std::string runtime = "unknown";
    std::optional<double> size_gb;
    nlohmann::json recipe_options = nlohmann::json::object();
};

bool launch_uses_tui(bool terminal, std::optional<bool> override,
                     const std::string& agent, const std::string& model);
std::vector<LaunchModel> parse_launch_models(const nlohmann::json& response,
                                           const nlohmann::json& health);
std::string launch_preset_url(const std::string& repo_dir, const std::string& recipe_file);
LaunchModel parse_launch_preset(const nlohmann::json& payload);
LaunchModel reconcile_launch_preset(LaunchModel preset, const std::vector<LaunchModel>& models);
std::vector<int> filter_launch_models(const std::vector<LaunchModel>& models,
                                      LaunchTab tab, const std::string& query,
                                      const std::string& agent);
std::string launch_model_warning(const LaunchModel& model, const std::string& agent);
bool apply_launch_context(const std::string& value, nlohmann::json& options,
                          std::string& error);

} // namespace lemon_cli
