#pragma once

#include "lemon/backends/backend_utils.h"
#include "lemon/backends/container_policy.h"

#include <optional>

namespace lemon::backends {

std::optional<ContainerPolicy> configured_container_policy(const BackendSpec& spec, const std::string& backend);
std::string effective_container_pin(const ContainerPolicy& policy, const nlohmann::json& versions, const std::string& requested);

}
