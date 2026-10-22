#pragma once

#include "lemon/backends/backend_registry.h"

namespace lemon::backends {

struct BuiltinRegistrationSeed {
    const BackendDescriptor* descriptor;
    std::string definition_id;
    BackendRegistration legacy;
};

const std::vector<BuiltinRegistrationSeed>& builtin_registration_seeds();

}
