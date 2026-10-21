#include "backend_definition_execution_fixture.h"
#include "lemon/backends/configured/backend_definition.h"

#include <type_traits>

using namespace execution_fixture;

template<class T, class = void>
struct CompilerAccepts : std::false_type {};
template<class T>
struct CompilerAccepts<T, std::void_t<decltype(compile_execution_definition(
    std::declval<T>(), std::declval<const ExecutionCompileLimits&>()))>> : std::true_type {};

int main() {
    static_assert(!std::is_default_constructible_v<CompiledDefinition>, "CompiledDefinitionIsNotPubliclyConstructible");
    static_assert(!std::is_constructible_v<CompiledDefinition, Json>);
    static_assert(!std::is_constructible_v<CompiledDefinition, ResolvedDefinition>);
    static_assert(!CompilerAccepts<std::shared_ptr<const ResolvedDefinition>>::value, "ResolvedDefinitionCannotBeCompilerInput");
    static_assert(!CompilerAccepts<Json>::value);
    static_assert(!CompilerAccepts<std::shared_ptr<const BackendDefinition>>::value, "V1DefinitionCannotBeCompilerInput");
    static_assert(!std::is_base_of_v<BackendDefinition, CompiledDefinition>);
    static_assert(!std::is_convertible_v<CompiledDefinition, BackendDefinition>, "NoConversionToV1Definition");
    static_assert(std::is_same_v<decltype(std::declval<CompiledDefinition&>().normalized_declaration()), const Json&>, "ConstViewsCannotMutateRecords");
    static_assert(std::is_same_v<decltype(std::declval<CompiledDefinition&>().option_bindings()), const std::vector<OptionBinding>&>);
    static_assert(std::is_same_v<decltype(std::declval<CompiledDefinition&>().operation_descriptions()), const std::vector<OperationDescription>&>);
    static_assert(std::is_same_v<decltype(ex::Inspection::program(std::declval<CompiledDefinition&>())), const ex::Program&>);
    check(code(compile_execution_definition(std::shared_ptr<const AuthorizedDefinition>{}), "invalid_authority"), "NullAuthorityRejected");
    const auto token = authorize(fixture());
    const auto first = compile_execution_definition(token), second = compile_execution_definition(token);
    check(first.definition && second.definition && &first.definition->normalized_declaration() != &second.definition->normalized_declaration() && &program(*first.definition).expressions != &program(*second.definition).expressions, "RepeatedCompilationDoesNotShareMutableState");
    auto detached = first.definition->normalized_declaration(); detached["metadata"]["log_category"] = "changed";
    check(second.definition->normalized_declaration()["metadata"]["log_category"] == "fake-engine" && token->definition().declaration["metadata"].contains("log_category") == false, "DetachedCopyCannotMutateRecords");
    Json child = {{"schema_version", 2}, {"id", "child"}, {"recipe", "child"}, {"extends", {{"builtin", "test-export"}, {"contract", 1}}}};
    auto base_document = Json::parse(read_bytes("export.synthetic.json"));
    base_document["merge"]["artifacts"] = Json::object();
    base_document["merge"]["operations"][0]["request"].erase("hooks");
    base_document["merge"]["operations"][0].erase("response");
    const auto base = base_document.dump(), bytes = child.dump();
    const auto sources = make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "test-export", 1}, base, ""}});
    const auto resolved = DefinitionResolver(sources.snapshot).resolve({DefinitionNamespace::Builtin, "test-export", 1}).definition;
    auto policy = contract();
    policy.slots.erase(std::remove_if(policy.slots.begin(), policy.slots.end(), [](const auto& slot) { return slot.slot.owner_kind != SlotOwnerKind::Root; }), policy.slots.end());
    policy.expected_source_digest = resolved->source_digest;
    policy.expected_definition_hash = resolved->definition_hash;
    policy.expected_dependency_digest = resolved->dependency_digest;
    repin(policy);
    const auto registry = BuiltinCatalogLoader::test_exports({policy}, sources.snapshot, installed_hook_abis(), v2::export_registry_digest({policy}, installed_hook_abis()));
    const auto frozen = BuiltinCatalogLoader::test_freeze({{{DefinitionNamespace::Builtin, "test-export", 1}, base, ""}, {{DefinitionNamespace::External, "child", 0}, bytes, ""}}, registry);
    const auto authorized = DefinitionAuthorizer(frozen.snapshot).authorize({DefinitionNamespace::External, "child", 0});
    check(bool(authorized.definition), "HookFixtureUsesRealAuthorization");
    if (!authorized.definition) return 1;
    const auto original = authorized.definition->definition().declaration;
    const auto rejected = compile_execution_definition(authorized.definition);
    check(code(rejected, "unsupported_execution_feature"), "HookBearingAuthorizedInputRejectedWithoutLookup");
    check(authorized.definition->definition().declaration == original, "HookFailurePreservesAuthoredDeclaration");
    check(evaluate({{"literal", {{"hook", {{"id", "not-installed"}}}}}}) == Json({{"hook", {{"id", "not-installed"}}}}), "LiteralHookTextDoesNotSelectTarget");
    return finish();
}
