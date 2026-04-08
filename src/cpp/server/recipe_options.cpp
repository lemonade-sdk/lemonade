#include <lemon/recipe_options.h>
#include <lemon/system_info.h>
#include <nlohmann/json.hpp>
#include <map>
#ifdef LEMONADE_CLI
#include <CLI/CLI.hpp>
#endif

namespace lemon {

using json = nlohmann::json;

static const json DEFAULTS = {
    {"ctx_size", 4096},
    {"llamacpp_backend", ""},  // "" means auto-detect (mapped from "auto" in config.json)
    {"llamacpp_args", ""},
    {"sd-cpp_backend", ""},   // "" means auto-detect (mapped from "auto" in config.json)
    {"sdcpp_args", ""},
    {"whispercpp_backend", ""},  // "" means auto-detect (mapped from "auto" in config.json)
    {"whispercpp_args", ""},
    // Image generation defaults (for sd-cpp recipe)
    // These are recipe-level defaults only, not CLI arguments — per reviewer guidance,
    // there are too many image gen params for CLI flags, and no universal defaults.
    {"steps", 20},
    {"cfg_scale", 7.0},
    {"width", 512},
    {"height", 512},
    {"sampling_method", ""},
    {"flow_shift", 0.0},
    // FLM-specific options
    {"flm_args", ""}       // Custom arguments to pass to flm serve
};

// Mapping from flat option names to CLI flags (used by to_cli_options)
// Note: Image generation params (steps, cfg_scale, width, height, sampling_method,
// flow_shift) are recipe-level defaults only — not exposed as CLI arguments.
// Runtime options (diffusion_fa, offload_to_cpu) go through --sdcpp-args.
static const std::map<std::string, std::string> OPTION_TO_CLI_FLAG = {
    {"ctx_size", "--ctx-size"},
    {"llamacpp_backend", "--llamacpp"},
    {"llamacpp_args", "--llamacpp-args"},
    {"sd-cpp_backend", "--sdcpp"},
    {"sdcpp_args", "--sdcpp-args"},
    {"whispercpp_backend", "--whispercpp"},
    {"whispercpp_args", "--whispercpp-args"},
    {"flm_args", "--flm-args"}
};

static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") {
        return {"ctx_size", "llamacpp_backend", "llamacpp_args"};
    } else if (recipe == "whispercpp") {
        return {"whispercpp_backend", "whispercpp_args"};
    } else if (recipe == "flm") {
        return {"ctx_size", "flm_args"};
    } else if (recipe == "ryzenai-llm") {
        return {"ctx_size"};
    } else if (recipe == "sd-cpp") {
        return {"sd-cpp_backend", "sdcpp_args", "steps", "cfg_scale", "width", "height", "sampling_method", "flow_shift"};
    } else {
        return {};
    }
}

static bool is_empty_option(json option) {
    return (option.is_number() && (option == -1)) ||
           (option.is_string() && (option == "" || option == "auto"));
}

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

std::vector<std::string> RecipeOptions::to_cli_options(const json& raw_options) {
    std::vector<std::string> cli;

    for (auto& [opt_name, cli_flag] : OPTION_TO_CLI_FLAG) {
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
    for (auto& [key, value] : DEFAULTS.items()) {
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
    if (opt.is_boolean()) return opt.get<bool>() ? "1" : "0";
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

    for (auto it = options.options_.begin(); it != options.options_.end(); ++it) {
        if (!merged.contains(it.key()) && !is_empty_option(it.value())) {
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
    return DEFAULTS.contains(opt) ? DEFAULTS[opt] : json();
}

#ifdef LEMONADE_CLI
// CLI_OPTIONS used only by the lemonade CLI client for add_cli_options
static const json CLI_OPTIONS = {
    {"--ctx-size", {{"option_name", "ctx_size"}, {"type_name", "SIZE"}, {"envname", "LEMONADE_CTX_SIZE"}, {"help", "Context size for the model"}}},
    {"--llamacpp", {{"option_name", "llamacpp_backend"}, {"type_name", "BACKEND"}, {"envname", "LEMONADE_LLAMACPP"}, {"help", "LlamaCpp backend to use"}}},
    {"--llamacpp-args", {{"option_name", "llamacpp_args"}, {"type_name", "ARGS"}, {"envname", "LEMONADE_LLAMACPP_ARGS"}, {"help", "Custom arguments to pass to llama-server"}}},
    {"--sdcpp", {{"option_name", "sd-cpp_backend"}, {"type_name", "BACKEND"}, {"envname", "LEMONADE_SDCPP"}, {"help", "SD.cpp backend to use"}}},
    {"--sdcpp-args", {{"option_name", "sdcpp_args"}, {"type_name", "ARGS"}, {"envname", "LEMONADE_SDCPP_ARGS"}, {"help", "Custom arguments to pass to sd-server (must not conflict with managed args)"}}},
    {"--whispercpp", {{"option_name", "whispercpp_backend"}, {"type_name", "BACKEND"}, {"envname", "LEMONADE_WHISPERCPP"}, {"help", "WhisperCpp backend to use"}}},
    {"--whispercpp-args", {{"option_name", "whispercpp_args"}, {"type_name", "ARGS"}, {"envname", "LEMONADE_WHISPERCPP_ARGS"}, {"help", "Custom arguments to pass to whisper-server"}}},
    // Note: Image gen params (--steps, --cfg-scale, --width, --height) removed — recipe-level only.
    // Runtime options (--diffusion-fa, --offload-to-cpu) go through --sdcpp-args.
    {"--flm-args", {{"option_name", "flm_args"}, {"type_name", "ARGS"}, {"envname", "LEMONADE_FLM_ARGS"}, {"help", "Custom arguments to pass to flm serve"}}}
};

void RecipeOptions::add_cli_options(CLI::App& app, json& storage) {
    for (auto& [key, opt] : CLI_OPTIONS.items()) {
        const std::string opt_name = opt["option_name"];
        CLI::Option* o;
        json defval = DEFAULTS[opt_name];

        if (defval.is_number_float()) {
            o = app.add_option_function<double>(key, [opt_name, &storage = storage](double val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((double) defval);
        } else if (defval.is_number_integer()) {
            o = app.add_option_function<int>(key, [opt_name, &storage = storage](int val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((int) defval);
        } else {
            o = app.add_option_function<std::string>(key, [opt_name, &storage = storage](const std::string& val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val(defval);
        }

        o->envname(opt["envname"]);
        o->type_name(opt["type_name"]);
    }
}
#endif

}
