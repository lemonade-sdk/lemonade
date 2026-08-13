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
    void set_option(const std::string& opt, const json& value);
    void remove_option(const std::string& opt);
    std::string get_recipe() const { return recipe_; };

#ifdef LEMONADE_CLI
    /// Add recipe options as CLI flags (used by lemonade CLI client only)
    static void add_cli_options(CLI::App& app, json& storage);
#endif
    static std::vector<std::string> to_cli_options(const json& raw_options);
    static std::vector<std::string> known_keys();

    /// Option names this recipe accepts, in declaration order.
    static std::vector<std::string> keys_for_recipe(const std::string& recipe);

    /// True for values the constructor drops as "not set". Generally null, -1,
    /// "" and "auto"; for ctx_size only null and "", since -1 is the storable
    /// value that means "size the context automatically".
    static bool is_default_sentinel(const std::string& key, const json& value);
private:
    json options_ = json::object();
    std::string recipe_ = "";
};
}
