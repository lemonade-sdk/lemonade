#pragma once

#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/configured/backend_definition_composition.h"

namespace lemon::backends {

struct DescriptorPlatform {
    std::string os;
    static DescriptorPlatform native();
};

BackendDescriptor descriptor_from_declaration(
    const nlohmann::json& declaration,
    DefinitionNamespace origin,
    const DescriptorPlatform& platform);

}
