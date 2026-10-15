#pragma once

#include "lemon_cli/lemonade_client.h"

#include <optional>
#include <set>

namespace lemon_cli {

nlohmann::json parse_generic_options(const std::vector<std::string>& assignments, const std::set<std::string>& explicit_names);
std::string read_backend_definition_file(const std::string& path);
int validate_backend_definition_offline(const std::string& bytes);
int validate_backend_definition(lemonade::LemonadeClient& client, const std::string& bytes);
int print_backend_schema(lemonade::LemonadeClient& client, const std::optional<std::string>& recipe);
int preflight_generic_options(lemonade::LemonadeClient& client, const std::string& model, const nlohmann::json& options);

}
