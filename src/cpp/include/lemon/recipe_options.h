#pragma once

#include <nlohmann/json.hpp>
#include <CLI/CLI.hpp>

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

    static void add_cli_options(CLI::App& app, json& storage);
private:
    json options_ = json::object();
    std::string recipe_ = "";
};
}