#pragma once

#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

class RecipeOptions {
public:
    RecipeOptions() {};
    RecipeOptions(const std::string& recipe, const json& options);
    json to_json() const;
    std::string to_log_string() const;
    RecipeOptions inherit(const RecipeOptions& options) const;
    json get_option(const std::string& opt) const;
private:
    json options_ = json::object();
    std::string recipe_ = "";
};
}