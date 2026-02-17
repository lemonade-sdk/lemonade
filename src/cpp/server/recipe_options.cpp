#include <lemon/recipe_options.h>
#include <lemon/system_info.h>
#include <nlohmann/json.hpp>
#include <map>

namespace lemon {

using json = nlohmann::json;

static const json DEFAULTS = {
    {"ctx_size", 4096},
#ifdef __APPLE__
    {"llamacpp_backend", "metal"},  // Will be overridden dynamically
#else
    {"llamacpp_backend", "vulkan"},  // Will be overridden dynamically
#endif
    {"llamacpp_args", ""},
    {"sd-cpp_backend", "cpu"},  // sd.cpp backend selection (cpu or rocm)
    {"whispercpp_backend", "npu"},
    // Image generation defaults (for sd-cpp recipe)
    {"steps", 20},
    {"cfg_scale", 7.0},
    {"width", 512},
    {"height", 512}
};

// CLI_OPTIONS without allowed_values for inference engines (will be set dynamically)
static const json CLI_OPTIONS = {
    // LLM Options
    {"--ctx-size", {
        {"option_name", "ctx_size"},
        {"type_name", "SIZE"},
        {"envname", "LEMONADE_CTX_SIZE"},
        {"help", "Context size for the model"}
    }},
    {"--llamacpp", {
        {"option_name", "llamacpp_backend"},
        {"type_name", "BACKEND"},
        {"envname", "LEMONADE_LLAMACPP"},
        {"help", "LlamaCpp backend to use"}
    }},
    {"--llamacpp-args", {
        {"option_name", "llamacpp_args"},
        {"type_name", "ARGS"},
        {"envname", "LEMONADE_LLAMACPP_ARGS"},
        {"help", "Custom arguments to pass to llama-server (must not conflict with managed args)"}
    }},
    // sd.cpp backend selection option
    {"--sdcpp", {
        {"option_name", "sd-cpp_backend"},
        {"type_name", "BACKEND"},
        {"allowed_values", {"cpu", "rocm"}},
        {"envname", "LEMONADE_SDCPP"},
        {"help", "SD.cpp backend to use (cpu for CPU, rocm for AMD GPU)"}
    }},
    // ASR options
    {"--whispercpp", {
        {"option_name", "whispercpp_backend"},
        {"type_name", "BACKEND"},
        {"allowed_values", {"cpu", "npu"}},
        {"envname", "LEMONADE_WHISPERCPP"},
        {"help", "WhisperCpp backend to use"}
    }},
    // Image generation options (for sd-cpp recipe)
    {"--steps", {
        {"option_name", "steps"},
        {"type_name", "N"},
        {"envname", "LEMONADE_STEPS"},
        {"help", "Number of inference steps for image generation"}
    }},
    {"--cfg-scale", {
        {"option_name", "cfg_scale"},
        {"type_name", "SCALE"},
        {"envname", "LEMONADE_CFG_SCALE"},
        {"help", "Classifier-free guidance scale for image generation"}
    }},
    {"--width", {
        {"option_name", "width"},
        {"type_name", "PX"},
        {"envname", "LEMONADE_WIDTH"},
        {"help", "Image width in pixels"}
    }},
    {"--height", {
        {"option_name", "height"},
        {"type_name", "PX"},
        {"envname", "LEMONADE_HEIGHT"},
        {"help", "Image height in pixels"}
    }},
};

static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") {
        return {"ctx_size", "llamacpp_backend", "llamacpp_args"};
    } else if (recipe == "whispercpp") {
        return {"whispercpp_backend"};
    } else if (recipe == "ryzenai-llm" || recipe == "flm") {
        return {"ctx_size"};
    } else if (recipe == "sd-cpp") {
        return {"sd-cpp_backend", "steps", "cfg_scale", "width", "height"};
    } else {
        return {};
    }
}

static const bool is_empty_option(json option) {
    return (option.is_number() && (option == -1)) ||
           (option.is_string() && (option == ""));
}

void RecipeOptions::add_cli_options(CLI::App& app, json& storage) {
    // Cache for supported backends per recipe (computed once per recipe)
    static std::map<std::string, SystemInfo::SupportedBackendsResult> backend_cache;

    for (auto& [key, opt] : CLI_OPTIONS.items()) {
        const std::string opt_name = opt["option_name"];
        CLI::Option* o;
        json defval = DEFAULTS[opt_name];

        // Generic handling for any *_backend option
        // Pattern: {recipe}_backend -> get supported backends for {recipe}
        const std::string backend_suffix = "_backend";
        bool is_backend_option = opt_name.size() > backend_suffix.size() &&
            opt_name.compare(opt_name.size() - backend_suffix.size(), backend_suffix.size(), backend_suffix) == 0;

        if (is_backend_option) {
            // Extract recipe name (everything before "_backend")
            std::string recipe = opt_name.substr(0, opt_name.size() - backend_suffix.size());

            // Get supported backends (cached)
            if (backend_cache.find(recipe) == backend_cache.end()) {
                backend_cache[recipe] = SystemInfo::get_supported_backends(recipe);
            }
            const auto& result = backend_cache[recipe];
            std::string default_backend = result.backends.empty() ? "" : result.backends[0];

            // Pre-populate storage with the dynamically detected default so it's
            // available even when the user doesn't explicitly pass the flag.
            // (add_option_function's callback only fires on explicit CLI input,
            // and default_val only affects help text display.)
            if (!default_backend.empty()) {
                storage[opt_name] = default_backend;
            }

            o = app.add_option_function<std::string>(key, [opt_name, &storage = storage](const std::string& val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val(default_backend);
            o->check(CLI::IsMember(result.backends));
        } else if (defval.is_number_float()) {
            o = app.add_option_function<double>(key, [opt_name, &storage = storage](double val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((double) defval);
        } else if (defval.is_number_integer()) {
            o = app.add_option_function<int>(key, [opt_name, &storage = storage](int val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val((int) defval);
        } else {
            o = app.add_option_function<std::string>(key, [opt_name, &storage = storage](const std::string& val) { storage[opt_name] = val; }, opt["help"]);
            o->default_val(defval);
        }

        // Common settings for all options
        o->envname(opt["envname"]);
        o->type_name(opt["type_name"]);
        if (opt.contains("allowed_values")) {
            o->check(CLI::IsMember(opt["allowed_values"].get<std::vector<std::string>>()));
        }
    }
}

std::vector<std::string> RecipeOptions::to_cli_options(const json& raw_options) {
    std::vector<std::string> cli;

    for (auto& [key, opt] : CLI_OPTIONS.items()) {
        const std::string opt_name = opt["option_name"];
        if (raw_options.contains(opt_name)) {
            auto val = raw_options[opt_name];
            if (val != "") {
                cli.push_back(key);
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

RecipeOptions::RecipeOptions(const std::string& recipe, const json& options) {
    recipe_ = recipe;
    std::vector<std::string> to_copy = get_keys_for_recipe(recipe_);

    for (auto key : to_copy) {
        if (options.contains(key) && !is_empty_option(options[key])) {
            options_[key] = options[key];
        }
    }
}

static const std::string inherit_string(const std::string& a, const std::string& b) {
    return a.empty() ? a : b;
}

static const int inherit_int(int a, int b) {
    return a != -1 ? a : b;
}

static std::string format_option_for_logging(const json& opt) {
    if (opt.is_number_float()) return std::to_string((double) opt);
    if (opt.is_number_integer()) return std::to_string((int) opt);
    if (opt == "") return "(none)";
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
    return options_.contains(opt) ? options_[opt] : DEFAULTS[opt];
}
}
