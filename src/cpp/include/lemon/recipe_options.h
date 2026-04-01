#pragma once

#include <nlohmann/json.hpp>
#ifdef LEMONADE_CLI
#include <CLI/CLI.hpp>
#endif

namespace lemon {

using json = nlohmann::json;

class RecipeOptions {
public:
    RecipeOptions() {};
    RecipeOptions(const std::string& recipe, const json& options);
    json to_json() const;
    std::string to_log_string(bool resolve_defaults=true) const;
    RecipeOptions inherit(const RecipeOptions& options) const;
    json get_option(const std::string& opt) const;
    std::string get_recipe() const { return recipe_; };

#ifdef LEMONADE_CLI
    /// Add recipe options as CLI flags (used by lemonade CLI client only)
    static void add_cli_options(CLI::App& app, json& storage);
#endif
    static std::vector<std::string> to_cli_options(const json& raw_options);
    static std::vector<std::string> known_keys();
private:
    json options_ = json::object();
    std::string recipe_ = "";
};
}
