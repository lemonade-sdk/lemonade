#pragma once

#include "lemon/backends/backend_registry.h"

namespace lemon::backends {

inline std::string builtin_container_selector_id(const std::string& recipe, const std::string& backend) {
    if (recipe == "llamacpp" && backend == "halobox") return "halobox";
    if (recipe == "llamacpp" && backend == "nathanw") return "nathanw";
    return "";
}

const BackendRegistration* select_backend_registration(const std::vector<BackendRegistration>& registrations,
    const std::string& recipe, const std::string& backend);

}
