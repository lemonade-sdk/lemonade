#include "backend_definition_binding_fixture.h"

#include <type_traits>

using namespace binding_fixture;

namespace {
template<class T, class = void> struct serializable : std::false_type {};
template<class T> struct serializable<T, std::void_t<decltype(Json(std::declval<T>()))>> : std::true_type {};
template<class T, class = void> struct invokes : std::false_type {};
template<class T> struct invokes<T, std::void_t<decltype(std::declval<T>().invoke())>> : std::true_type {};
template<class T, class = void> struct targets : std::false_type {};
template<class T> struct targets<T, std::void_t<decltype(std::declval<T>().targets())>> : std::true_type {};
}

int main() {
    using Bind = decltype(&bind_execution_definition);
    static_assert(std::is_same_v<Bind, ExecutionBindResult (*)(std::shared_ptr<const AdmittedCatalog>,
        std::shared_ptr<const CompiledDefinition>, const ExecutionBindLimits&)>);
    static_assert(std::is_final_v<ExecutableDefinition>);
    static_assert(!std::is_copy_constructible_v<ExecutableDefinition> && !std::is_copy_assignable_v<ExecutableDefinition>);
    static_assert(!std::is_default_constructible_v<ExecutableDefinition>);
    static_assert(!std::is_constructible_v<ExecutableDefinition, Json>);
    static_assert(!std::is_constructible_v<ExecutableDefinition, std::shared_ptr<const CompiledDefinition>>);
    static_assert(!std::is_invocable_v<Bind, Json, Json, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, std::shared_ptr<const ResolvedDefinition>, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, std::shared_ptr<const BackendDefinition>, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, std::shared_ptr<const AuthorizedDefinition>, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, typed::Registration, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, typed::Target, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, std::string, ExecutionBindLimits>);
    static_assert(!std::is_invocable_v<Bind, std::shared_ptr<const AdmittedCatalog>, std::shared_ptr<const CompiledDefinition>, AuthorizedDefinition, ExecutionBindLimits>);
    static_assert(!serializable<ExecutableDefinition>::value && !invokes<ExecutableDefinition>::value && !targets<ExecutableDefinition>::value);
    static_assert(std::is_same_v<decltype(std::declval<ExecutableDefinition>().compiled()), const CompiledDefinition&>);
    static_assert(std::is_same_v<decltype(std::declval<ExecutableDefinition>().metrics()), const ExecutionBindMetrics&>);
    static_assert(std::is_same_v<decltype(std::declval<ExecutableDefinition>().execution_id()), const std::string&>);
    check(true, "ExactBinderAPIAndImmutableBoundaryCompileTimeAssertions");
    try {
        auto value = fixture();
        const auto bound = bind(value);
        check(bool(bound.definition), "ImmutableBoundaryHookBearingBind");
        if (!bound.definition) return finish();
        const auto* original_compiled = value.compiled.get();
        const auto* original_catalog = value.admitted.get();
        const auto* original_token = value.token.get();
        std::weak_ptr<const AdmittedCatalog> catalog = value.admitted;
        std::weak_ptr<const CompiledDefinition> compiled = value.compiled;
        std::weak_ptr<const AuthorizedDefinition> token = value.token;
        value = {};
        check(!catalog.expired() && !compiled.expired() && !token.expired() && &bound.definition->compiled() == original_compiled &&
            &bd::Inspection::admitted(*bound.definition) == original_catalog && &bound.definition->compiled().authorized() == original_token,
            "BoundObjectRetainsExactInputs");
        check(!bind_execution_definition({}, {}).definition && !bind_execution_definition(catalog.lock(), {}).definition &&
            !bind_execution_definition({}, compiled.lock()).definition, "NullInputsFailClosed");
        check(embedded_v2_sources().empty() && embedded_v2_policies().leaves.empty() && installed_builtin_exports().exports().empty(),
            "ProductionV2ExecutionTablesStillEmpty");
        zero("boundary success and failure");
    } catch (const std::exception& error) { check(false, error.what()); }
    return finish();
}
