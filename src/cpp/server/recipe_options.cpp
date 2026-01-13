#include <lemon/recipe_options.h>
#include <nlohmann/json.hpp>

#include <iostream>

namespace lemon {

using json = nlohmann::json;

static const json DEFAULTS = {{"ctx_size", 4096}, {"llamacpp_backend", "vulkan"}, {"llamacpp_args", ""}};

static std::vector<std::string> get_keys_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") {
        return {"ctx_size", "llamacpp_backend", "llamacpp_args"};
    } else if (recipe == "oga-npu" || recipe == "oga-hybrid" || recipe == "oga-cpu" || recipe == "ryzenai" || recipe == "flm") {
        return {"ctx_size"};
    } else {
        // "whispercpp" has currently no option
        return {};
    }
}

static const bool is_empty_option(json option) {
    return (option.is_number() && (option == -1)) || 
           (option.is_string() && (option == ""));
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
    if (opt.is_number()) return std::to_string((int) opt);
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