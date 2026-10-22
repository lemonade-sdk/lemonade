#include "backend_builtin_source_resources.h"
#include "backend_definition_exports_internal.h"

namespace lemon::backends {

const std::vector<DefinitionSourceView>& embedded_v2_sources() {
    return builtin_source_views();
}

const BuiltinExecutionPolicies& embedded_v2_policies() {
    return fixed_builtin_v2_policies();
}

}
