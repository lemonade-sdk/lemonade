#include <lemon/recipe_options.h>
#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/utils/custom_args.h>
#include <nlohmann/json.hpp>
#include <map>
#ifdef LEMONADE_CLI
#include <CLI/CLI.hpp>
#else
#include <lemon/system_info.h>
#endif

namespace lemon {

using json = nlohmann::json;

// Options shared by every backend. Per-backend options (and ctx_size opt-in)
// come from each backend's descriptor; these are the universal kit.
static const json& common_defaults() {
    static const json d = {
        {"ctx_size", -1},  // -1 triggers auto-resolution (memory + arch metadata)
        {"merge_args", true},
        // Auto-eviction options (apply to every recipe)
        {"auto_evict", nullptr},          // nullptr means fallback to global config
        {"evict_idle_timeout", 300},      // Default hard idle timeout (5 mins)
        {"downsize_idle_timeout", 60},    // Default soft idle timeout (1 min)
        {"evict_weight_factor", 1.0},     // Eviction-protection weight (higher = more protected)
        {"pinned", false},
    };
    return d;
}

// Defaults for every option: the common kit plus each backend descriptor's
// declared options. Built once from the registry so config defaults, CLI flags,
// and load-time resolution can never drift from the descriptors.
static const json& get_defaults() {
    static const json defaults = [] {
        json d = common_defaults();
        for (const auto* desc : lemon::backends::all_descriptors()) {
            for (const auto& opt : desc->options) {
                d[opt.name] = opt.default_value;
            }
        }
        return d;
    }();
    return defaults;
}

// Flat option name -> CLI flag, for to_cli_options(). ctx_size/merge_args are
// the common flags; the rest come from descriptor options that declare a flag.
static const std::map<std::string, std::string>& get_option_to_cli_flag() {
    static const std::map<std::string, std::string> mapping = [] {
        std::map<std::string, std::string> m{
            {"ctx_size", "--ctx-size"},
            {"merge_args", "--merge-args"},
        };
        for (const auto* desc : lemon::backends::all_descriptors()) {
            for (const auto& opt : desc->options) {
                if (!opt.cli_flag.empty()) {
                    m[opt.name] = opt.cli_flag;
                }
            }
        }
        return m;
    }();
    return mapping;
}

static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    std::vector<std::string> keys;
    if (const auto* desc = lemon::backends::descriptor_for(recipe)) {
        if (desc->uses_ctx_size) {
            keys.push_back("ctx_size");
        }
        for (const auto& opt : desc->options) {
            keys.push_back(opt.name);
        }
        keys.push_back("merge_args");
    }

    // Add auto-eviction options for all recipes
    keys.push_back("auto_evict");
    keys.push_back("evict_idle_timeout");
    keys.push_back("downsize_idle_timeout");
    keys.push_back("evict_weight_factor");
    keys.push_back("pinned");

    return keys;
}

static bool is_empty_option(json option) {
    return option.is_null() ||
           (option.is_number() && (option == -1)) ||
           (option.is_string() && (option == "" || option == "auto"));
}


#ifndef LEMONADE_CLI
static bool try_get_backend_options(const std::string& opt_name, SystemInfo::SupportedBackendsResult& result) {
    // Generic handling for any *_backend option
    // Pattern: {recipe}_backend -> get supported backends for {recipe}
    const std::string backend_suffix = "_backend";
    bool is_backend_option = opt_name.size() > backend_suffix.size() &&
        opt_name.compare(opt_name.size() - backend_suffix.size(), backend_suffix.size(), backend_suffix) == 0;

    if (is_backend_option) {
        // Extract recipe name (everything before "_backend")
        std::string recipe = opt_name.substr(0, opt_name.size() - backend_suffix.size());
        auto tmp = SystemInfo::get_supported_backends(recipe);
        result.backends = tmp.backends;
    }

    return is_backend_option;
}
#endif

std::vector<std::string> RecipeOptions::to_cli_options(const json& raw_options) {
    std::vector<std::string> cli;

    for (auto& [opt_name, cli_flag] : get_option_to_cli_flag()) {
        if (raw_options.contains(opt_name)) {
            auto val = raw_options[opt_name];
            if (!val.is_null() && val != "") {
                cli.push_back(cli_flag);
                if (val.is_number_float()) {
                    cli.push_back(std::to_string((double) val));
                } else if (val.is_number_integer()) {
                    cli.push_back(std::to_string((int) val));
                } else {
                    cli.push_back(val);
                }
            }
        }
    }

    return cli;
}

std::vector<std::string> RecipeOptions::known_keys() {
    std::vector<std::string> keys;
    for (auto& [key, value] : get_defaults().items()) {
        keys.push_back(key);
    }
    return keys;
}

RecipeOptions::RecipeOptions(const std::string& recipe, const json& options) {
    recipe_ = recipe;
    std::vector<std::string> to_copy = get_keys_for_recipe(recipe_);

    for (auto key : to_copy) {
        if (options.contains(key) && !is_empty_option(options[key])) {
            options_[key] = options[key];
        }
    }
}

static std::string format_option_for_logging(const json& opt) {
    if (opt.is_null() || opt == "") return "(none)";
    if (opt.is_boolean()) return opt.get<bool>() ? "true" : "false";
    if (opt.is_number_float()) return std::to_string((double) opt);
    if (opt.is_number_integer()) return std::to_string((int) opt);
    return opt;
}

json RecipeOptions::to_json() const {
    return options_;
}

std::string RecipeOptions::to_log_string(bool resolve_defaults) const {
    std::vector<std::string> to_log = get_keys_for_recipe(recipe_);
    std::string log_string = "";
    bool first = true;
    for (auto key : to_log) {
        if (resolve_defaults || options_.contains(key)) {
            if (!first) log_string += ", ";
            first = false;
            log_string += key + "=" + format_option_for_logging(get_option(key));
        }
    }

    return log_string;
}

RecipeOptions RecipeOptions::inherit(const RecipeOptions& options) const {
    json merged = options_;
    bool merge_args = options_.contains("merge_args") ? options_["merge_args"].get<bool>() : options.get_option("merge_args").get<bool>();

    for (auto it = options.options_.begin(); it != options.options_.end(); ++it) {
        if (merge_args && it.key().size() >= 5 && it.key().substr(it.key().size() - 5) == "_args") {
            // Special handling for _args options: parse, merge maps, re-stringify
            std::string target_str = "";
            if (merged.contains(it.key()) && merged[it.key()].is_string()) {
                target_str = merged[it.key()];
            }

            std::string incoming_str = it.value().is_string() ? it.value() : "";

            if (target_str.empty()) {
                merged[it.key()] = incoming_str;
            } else if (incoming_str.empty()) {
                merged[it.key()] = target_str;
            } else {
                auto target_tokens = lemon::utils::parse_custom_args(target_str, true);
                auto incoming_tokens = lemon::utils::parse_custom_args(incoming_str, true);

                auto target_map = lemon::utils::build_custom_args_map(target_tokens);
                auto incoming_map = lemon::utils::build_custom_args_map(incoming_tokens);

                auto merged_map = lemon::utils::merge_args_maps(target_map, incoming_map);
                merged[it.key()] = lemon::utils::map_to_args_string(merged_map);
            }
        } else if (!merged.contains(it.key()) && !is_empty_option(it.value())) {
            merged[it.key()] = it.value();
        }
    }

    return RecipeOptions(recipe_, merged);
}

json RecipeOptions::get_option(const std::string& opt) const {
    if (options_.contains(opt)) {
        return options_[opt];
    }
#ifndef LEMONADE_CLI
    // Dynamic defaults for backends if not explicitly set
    SystemInfo::SupportedBackendsResult backend_result;
    if (try_get_backend_options(opt, backend_result)) {
        if (!backend_result.backends.empty()) {
            return backend_result.backends[0];
        }
    }
#endif
    return get_defaults().contains(opt) ? get_defaults()[opt] : json();
}

void RecipeOptions::set_option(const std::string& opt, const json& value) {
    options_[opt] = value;
}

void RecipeOptions::remove_option(const std::string& opt) {
    options_.erase(opt);
}

#ifdef LEMONADE_CLI
// CLI_OPTIONS used only by the lemonade CLI client for add_cli_options.
// ctx_size/merge_args are the common flags; everything else is derived from
// descriptor options that declare a CLI flag. Image-gen params
// (steps/cfg_scale/width/height) have no cli_flag in their descriptor, so they
// stay recipe-level only.
static const json& get_cli_options() {
    static const json cli_options = [] {
        json o = json::object();
        o["--ctx-size"] = {{"option_name", "ctx_size"}, {"type_name", "SIZE"}, {"help", "Context size for the model"}, {"group", "General Options"}};
        o["--merge-args"] = {{"option_name", "merge_args"}, {"type_name", "BOOL"}, {"help", "Merge global and model arguments when loading the model"}, {"group", "General Options"}};
        for (const auto* desc : lemon::backends::all_descriptors()) {
            for (const auto& opt : desc->options) {
                if (opt.cli_flag.empty()) {
                    continue;
                }
                json entry = {{"option_name", opt.name}, {"type_name", opt.type_name}, {"help", opt.help}};
                if (!opt.group.empty()) {
                    entry["group"] = opt.group;
                }
                o[opt.cli_flag] = entry;
            }
        }
        return o;
    }();
    return cli_options;
}

void RecipeOptions::add_cli_options(CLI::App& app, json& storage) {
    for (auto& [key, opt] : get_cli_options().items()) {
        const std::string opt_name = opt["option_name"];
        CLI::Option* o;
        json defval = get_defaults()[opt_name];

        if (defval.is_number_float()) {
            o = app.add_option_function<double>(key, [opt_name, &storage = storage](double val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((double) defval);
        } else if (defval.is_number_integer()) {
            o = app.add_option_function<int>(key, [opt_name, &storage = storage](int val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((int) defval);
        } else if (defval.is_boolean()) {
            o = app.add_flag_function(key + ",!" + lemon::utils::negate_flag(key), [opt_name, defval, &storage = storage](std::int64_t val) { storage[opt_name] = val == 0 ? defval.get<bool>() : val > 0; }, opt["help"]);
            o->default_val((bool) defval);
        } else {
            o = app.add_option_function<std::string>(key, [opt_name, &storage = storage](const std::string& val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val(defval);
        }

        o->type_name(opt["type_name"]);
        o->group(opt.value("group", "Options"));
    }
}
#endif

}
