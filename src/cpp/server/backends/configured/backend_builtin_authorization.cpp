#include "backend_builtin_source_resources.h"
#include "backend_definition_exports_internal.h"

namespace lemon::backends {

const std::vector<DefinitionSourceView>& embedded_v2_sources() {
    return builtin_source_views();
}

const BuiltinExecutionPolicies& embedded_v2_policies() {
    static const BuiltinExecutionPolicies policies{{}, "sha256:43df4f52e00eb4db57e0f0e9bd02ed60293466cc86a2303496335977d370de09"};
    return policies;
}

}
