#include "backend_builtin_source_resources.h"
#include "backend_definition_exports_internal.h"

namespace lemon::backends {

const std::vector<DefinitionSourceView>& embedded_v2_sources() {
    return builtin_source_views();
}

const BuiltinExecutionPolicies& embedded_v2_policies() {
    static const BuiltinExecutionPolicies policies{{}, "sha256:06038c6bac962c715a33f7b09827ee6b48be35a80917be6ade54357a0e96111d"};
    return policies;
}

}
