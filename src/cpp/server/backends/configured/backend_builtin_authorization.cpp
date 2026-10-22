#include "backend_builtin_source_resources.h"
#include "backend_definition_exports_internal.h"

namespace lemon::backends {

const std::vector<DefinitionSourceView>& embedded_v2_sources() {
    return builtin_source_views();
}

const BuiltinExecutionPolicies& embedded_v2_policies() {
    static const BuiltinExecutionPolicies policies{{}, "sha256:6bac770f2a8d740927eb20a69a430d9430a0eabea744efedb74486afe941ead6"};
    return policies;
}

}
