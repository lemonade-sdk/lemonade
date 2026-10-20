#include "backend_definition_v2_descriptor.h"

const lemon::BackendDescriptor* fixture_descriptor_peer() {
    return &lemon::backends::v2_fixture::descriptor;
}
