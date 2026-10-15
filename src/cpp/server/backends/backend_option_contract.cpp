#include "lemon/backends/backend_option_contract.h"

#include <algorithm>
#include <cmath>

namespace lemon::backends {

std::string backend_option_config_key(const std::string& name) {
    const auto suffix = [&](const std::string& ending) { return name.size() >= ending.size() && name.compare(name.size() - ending.size(), ending.size(), ending) == 0; };
    return suffix("_backend") ? "backend" : suffix("_args") ? "args" : suffix("_device") ? "device" : name;
}

bool option_is_unset(const nlohmann::json& contract, const nlohmann::json& value) {
    const auto values = contract.value("unset_values", nlohmann::json::array({nullptr}));
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string validate_option_contract(const nlohmann::json& contract, const nlohmann::json& value) {
    if (option_is_unset(contract, value)) return "";
    const auto type = contract.at("value_type").get<std::string>();
    const bool valid = type == "string" ? value.is_string() : type == "boolean" ? value.is_boolean() :
        type == "integer" ? value.is_number_integer() : type == "number" && value.is_number();
    if (!valid) return "Incorrect option value type";
    if (value.is_number() && !std::isfinite(value.get<double>())) return "Nonfinite option value";
    if (contract.contains("enum") && std::find(contract["enum"].begin(), contract["enum"].end(), value) == contract["enum"].end()) return "Option value absent from enum";
    if (contract.contains("minimum") && value < contract["minimum"]) return "Option value below minimum";
    if (contract.contains("maximum") && value > contract["maximum"]) return "Option value above maximum";
    return "";
}

}
