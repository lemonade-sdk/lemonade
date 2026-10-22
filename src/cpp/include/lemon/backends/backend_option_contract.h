#pragma once

#include "lemon/backends/backend_descriptor.h"

#include <map>
#include <memory>

namespace lemon::backends {

struct BackendCatalogData {
    std::vector<const BackendDescriptor*> descriptors;
    std::map<std::string, nlohmann::json> external_options;
};

std::string backend_option_config_key(const std::string& name);
bool option_is_unset(const nlohmann::json& contract, const nlohmann::json& value);
std::string validate_option_contract(const nlohmann::json& contract, const nlohmann::json& value);
const nlohmann::json* external_option_contract(const std::string& recipe, const std::string& name);
const nlohmann::json* external_option_contract(const std::string& name);
bool descriptor_snapshot_consumed();

}
