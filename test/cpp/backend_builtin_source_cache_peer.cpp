#include "backend_builtin_source_test_descriptor.h"

const lemon::BackendDescriptor* source_cache_descriptor_peer() {
    return &lemon::backends::source_cache_fixture::descriptor;
}

const lemon::backends::ResolvedDefinition* source_cache_resolved_peer() {
    return &lemon::backends::builtin_resolved_source({lemon::backends::DefinitionNamespace::Builtin, "source-cache-fixture", 1});
}
