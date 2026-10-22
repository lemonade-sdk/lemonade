#include "backend_builtin_source_resources.h"
#include "backend_definition_exports_internal.h"

namespace lemon::backends {

const std::vector<DefinitionSourceView>& embedded_v2_sources() {
    return builtin_source_views();
}

const BuiltinExecutionPolicies& embedded_v2_policies() {
    static const BuiltinExecutionPolicies policies{{}, "sha256:b2e770dfdb68a27db72cbe2fdad29f26c3eeb7ab2ca5ea136d325bfe4a5dfa8a"};
    return policies;
}

}
