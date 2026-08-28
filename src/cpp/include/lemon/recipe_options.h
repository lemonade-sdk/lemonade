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
    bool has_option(const std::string& opt) const;
    void set_option(const std::string& opt, const json& value);
    void remove_option(const std::string& opt);
    // True when this layer explicitly named or set this option, even if its
    // sentinel value was dropped from options_. inherit() preserves this provenance.
    bool has_explicit_option(const std::string& opt) const;
    json get_explicit_option(const std::string& opt) const;
    std::string get_recipe() const { return recipe_; };

#ifdef LEMONADE_CLI
    /// Add recipe options as CLI flags (used by lemonade CLI client only)
    static void add_cli_options(CLI::App& app, json& storage);
#endif
    static std::vector<std::string> to_cli_options(const json& raw_options);
    static std::vector<std::string> known_keys();

    /// Option names this recipe accepts, in declaration order.
    static std::vector<std::string> keys_for_recipe(const std::string& recipe);

    /// Fold precedence layers into one RecipeOptions. Layers are given lowest
    /// to highest precedence; each later layer's keys overwrite earlier ones
    /// wholesale, including *_args (which are only token-merged by inherit()
    /// between the Router's layers). Returns an empty RecipeOptions when all
    /// layers are null/absent.
    static RecipeOptions merge_precedence_layers(const std::string& recipe,
                                                 const json& lowest,
                                                 const json& middle,
                                                 const json& highest);

    /// Every option the recipe accepts, with unset keys resolved to their
    /// defaults. Two option sets that resolve equal describe the same load,
    /// however sparsely each one was spelled.
    json to_resolved_json() const;

    /// True for values the constructor drops as "not set": null, -1, "" and
    /// "auto". ctx_size is the exception — every non-number is dropped there,
    /// since -1 is the storable value meaning "size the context automatically".
    static bool is_default_sentinel(const std::string& key, const json& value);
private:
    json options_ = json::object();
    json explicit_options_ = json::object();
    std::string recipe_ = "";
};
}
