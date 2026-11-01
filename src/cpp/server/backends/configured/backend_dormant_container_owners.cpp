#include "backend_definition_exports_internal.h"

#include <stdexcept>

namespace lemon::backends {

const BuiltinContainerExecutionOwner& reviewed_container_execution_owner(const std::string& id) {
#include "dormant_container_owners.h"
    throw std::invalid_argument("Unknown dormant container execution owner: " + id);
}

}
