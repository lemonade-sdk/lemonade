#include "backend_definition_execution_fixture.h"

#include <filesystem>

using namespace execution_fixture;

int main() {
    const auto token = authorize(fixture());
    const auto authored = token->definition().declaration;
    const auto hash = token->definition().definition_hash;
    const auto source = token->definition().source_digest;
    const auto dependency = token->definition().dependency_digest;
    const auto provenance = token->definition().provenance;
    const auto origin = provenance->origin_at("/metadata/server_name");
    const auto result = compile_execution_definition(token);
    check(bool(result.definition), "AuthorizedInputCompiles");
    if (!result.definition) { for (const auto& d : result.diagnostics.entries) std::printf("%s %s\n", d.code.c_str(), d.pointer.c_str()); return 1; }
    const auto& compiled = *result.definition;
    const auto& doc = compiled.normalized_declaration();
    check(doc["metadata"].value("log_category", "") == "fake-engine" && doc["metadata"].value("log_level_policy", "") == "passthrough" && doc["metadata"].contains("options") && doc["metadata"]["options"].is_array() && doc["metadata"]["options"].empty() && doc["install"].contains("cases") && doc["install"]["cases"].is_array() && doc["install"]["cases"].empty(), "DefaultsOnlyOnExecutionCopy");
    check(token->definition().declaration == authored && !authored["metadata"].contains("log_category") && !authored["install"].contains("cases"), "AuthoredDeclarationUnchanged");
    check(token->definition().source_digest == source && token->definition().definition_hash == hash && token->definition().dependency_digest == dependency && token->definition().provenance == provenance && provenance->origin_at("/metadata/server_name")->pointer == origin->pointer, "AuthoredHashesAndProvenanceUnchanged");
    check(&compiled.authorized() == token.get(), "ExactAuthorityRetained");
    auto value = fixture();
    auto& data = value["merge"];
    data["metadata"]["options"] = Json::array({option(), option("arguments", "fork_args", ""), option("ctx", "fork_ctx", 16, "integer")});
    data["launch"]["backend"] = {{"option", "engine"}, {"fallback", "first_supported"}, {"on_empty", {{"operation", "run"}, {"target", "backend"}}}};
    data["launch"]["custom_args"] = {{"option", "arguments"}, {"reserved", Json::array()}, {"error_prefix", "bad"}, {"defaults", Json::array({
        {{"id", "runtime"}, {"args", "--threads 2"}, {"flag", "--threads"}, {"stage", "runtime_options"}, {"respect_merge_args", true}},
        {{"id", "launch"}, {"args", "--parallel 1"}, {"flag", "--parallel"}, {"stage", "launch"}, {"respect_merge_args", false}}})}};
    data["launch"]["argv"] = Json::array({{{"id", "raw"}, {"args", {"--raw", { {"option", "engine"} }}}},
        {{"id", "flag"}, {"flag", "--device"}, {"value", {{"option", "engine"}}}, {"aliases", {"-d"}}}});
    data["launch"]["environment"]["set"] = Json::array({{{"name", "Z_FIRST"}, {"value", "z"}}, {{"name", "A_SECOND"}, {"value", "a"}}});
    data["launch"]["context_floor"] = {{"llm", 4096}, {"embedding", 8192}};
    data["artifacts"]["optional"] = {{"role", "draft"}, {"strategy", "resolved_path"}, {"required_when", false}, {"missing_error", "missing"}};
    const auto bound = compile(value);
    check(bool(bound.definition), "CompleteLaunchCompiles");
    if (!bound.definition) return 1;
    const auto& ir = program(*bound.definition);
    check(bound.definition->option_bindings()[0].id == "engine" && bound.definition->option_bindings()[0].public_name == "fork_backend" && bound.definition->option_bindings()[0].config_key == "backend" && bound.definition->option_bindings()[0].type == "string" && ir.launch.backend_slot == 0 && ir.launch.custom_args_slot == 1, "StableIdsBindRenamedPublicOptions");
    ex::InvocationFrame frame;
    frame.options["fork_backend"] = "";
    const auto& expression = ir.expressions.at("/launch/argv/0/args/1");
    check(expression.evaluate(frame, ir.bindings) == Json(""), "ExplicitEmptyIsPreserved");
    check(expression.evaluate({}, ir.bindings) == Json("cpu"), "MissingUsesDeclaredDefault");
    frame.options["fork_backend"] = nullptr;
    check(expression.evaluate(frame, ir.bindings) == Json(nullptr), "OptionNullIsPresent");
    check(ir.launch.argv[1].reserve == "always" && ir.launch.argv[1].aliases == std::vector<std::string>{"-d"}, "FlagReservationsDefaultAlways");
    check(!ir.launch.argv[0].flag && ir.launch.argv[0].reserve.empty() && ir.launch.argv[0].values.size() == 2, "RawArgumentsRemainRaw");
    check(ir.launch.defaults.size() == 2 && ir.launch.defaults[0].stage == "runtime_options" && ir.launch.defaults[0].respect_merge_args && ir.launch.defaults[1].stage == "launch" && !ir.launch.defaults[1].respect_merge_args, "CustomDefaultsRetainStageAndMergePolicy");
    check(ir.expressions.at("/artifacts/optional/required_when").evaluate({}, ir.bindings) == Json(false), "OptionalArtifactRequirementCompiles");
    check(ir.launch.environment_set.size() == 2 && ir.launch.environment_set[0].name == "Z_FIRST" && ir.launch.environment_set[1].name == "A_SECOND", "EnvironmentAssignmentsPreserveOrder");
    check(ir.launch.context_floor.at(ModelType::LLM) == 4096 && ir.launch.context_floor.at(ModelType::EMBEDDING) == 8192, "ContextFloorUsesCanonicalModelType");
    check(ir.launch.working_directory.at("windows") == "executable_directory" && ir.launch.working_directory.at("linux") == "inherit" && ir.launch.working_directory.at("macos") == "inherit", "CwdMapRetainsAllPlatforms");
    frame.values["request"] = {{"null", nullptr}, {"empty", ""}, {"zero", 0}, {"false", false}};
    check(evaluate({{"ref", "/request/null"}, {"default", "fallback"}}, frame) == Json(nullptr), "ExpressionDefaultDoesNotReplaceNull");
    check(evaluate({{"ref", "/request/missing"}, {"default", "fallback"}}, frame) == Json("fallback"), "AbsentReferenceUsesDefault");
    for (const auto* key : {"empty", "zero", "false"}) check(evaluate({{"ref", std::string("/request/") + key}, {"default", "fallback"}}, frame) == frame.values["request"][key], std::string("PresentValuePreserved:") + key);
    check(evaluate({{"present", {{"ref", "/request/null"}}}}, frame) == Json(true) && evaluate({{"coalesce", Json::array({{{"ref", "/request/null"}}, "fallback"})}}, frame) == Json(nullptr), "PresentAndCoalescePreserveNull");
    const Json literal = {{"hook", {{"id", "PRIVATE_HOOK"}}}, {"option", "not-a-slot"}, {"ref", "/PRIVATE_PATH"}};
    check(evaluate({{"literal", literal}}) == literal && evaluate({{"ref", "/request/missing"}, {"default", literal}}) == literal, "LiteralPayloadIsOpaque");
    check(evaluate({{"concat", Json::array({"a", {{"string", 2}}})}}) == Json("a2"), "ConcatAndString");
    check(evaluate({{"all", Json::array({true, {{"not", false}}})}}) == Json(true) && evaluate({{"any", Json::array({false, true})}}) == Json(true), "BooleanExpressions");
    check(evaluate({{"gt", {2, 1}}}) == Json(true) && evaluate({{"eq", {1, 1}}}) == Json(true), "ComparisonExpressions");
    check(!evaluate({{"when", {false, "hidden"}}}) && evaluate({{"when", {true, "shown"}}}) == Json("shown"), "ConditionalExpression");
    check(evaluate({{"parent_path", "dir/file"}}) == Json("dir") && evaluate({{"path_join", {"dir", "file"}}}) == Json((std::filesystem::path("dir") / "file").string()), "LexicalPathExpressions");
    frame.values["options"]["ctx_size"] = 4096;
    check(evaluate({{"ref", "/options/ctx_size"}}, frame) == Json(4096), "InvocationOptionFactsRemainAvailable");
    check(evaluate({{"ref", "/request/zero"}, {"type", "number"}}, frame) == Json(0), "IntegerIsNumber");
    bool thrown = false;
    try { evaluate({{"ref", "/request/zero"}, {"type", "boolean"}}, frame); } catch (const std::invalid_argument&) { thrown = true; }
    check(thrown, "ExplicitTypeDoesNotCoerce");
    value = fixture(); value["merge"]["operations"].push_back(value["merge"]["operations"][0]); value["merge"]["operations"][1]["id"] = "other";
    check(code(compile(value), "operation_overlap"), "OperationCellsDoNotOverlap");
    value = fixture(); value["merge"]["operations"][0]["request"]["set"] = {{"/a", 1}, {"/a/b", 2}};
    check(code(compile(value), "mapping_overlap"), "MappingOverlapRejected");
    value = fixture(); value["merge"]["operations"][0]["transport"]["kind"] = "http_bytes";
    check(code(compile(value), "unsupported_execution_feature"), "UnsupportedKnownPrimitiveRejected");
    value = fixture(); value["merge"]["operations"][0]["delivery"] = "streaming"; value["merge"]["operations"][0]["transport"]["kind"] = "http_sse";
    check(bool(compile(value).definition), "OpenAISSECompiles");
    value = fixture(); value["merge"]["metadata"]["options"] = Json::array({option("bad", "bad", 2, "boolean")});
    check(code(compile(value), "invalid_option_contract"), "InvalidOptionDefaultRejected");
    value = fixture(); value["merge"]["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", {{"ref", "/launch/ports/http"}}}};
    check(code(compile(value), "phase_dependency"), "PortsUnavailableBeforeAllocation");
    value = fixture(); value["merge"]["operations"][0]["request"]["set"] = {{"/x", {{"ref", "/launch/hook/fact"}}}};
    check(code(compile(value), "unsupported_execution_feature"), "FutureHookFactsRejected");
    check(compile_execution_definition(token).definition->compilation_id() == compiled.compilation_id(), "CompilationIdentityIsDeterministic");
    ex::Budget budget{{}, {}};
    check(ex::compilation_identity(token->definition(), program(compiled), budget, ex::compiler_revision + 1) != compiled.compilation_id() && token->definition().definition_hash == hash, "CompilerRevisionChangesCompilationIdentityOnly");
    auto different_identity = token->definition();
    different_identity.definition_hash = "different-definition";
    ex::Budget identity_budget{{}, {}};
    check(ex::compilation_identity(different_identity, program(compiled), identity_budget) != compiled.compilation_id(), "DefinitionIdentityIsIncluded");
    different_identity = token->definition();
    different_identity.dependency_digest = "different-dependency";
    identity_budget = {{}, {}};
    check(ex::compilation_identity(different_identity, program(compiled), identity_budget) != compiled.compilation_id(), "DependencyIdentityIsIncluded");
    auto required_artifact = fixture();
    required_artifact["merge"]["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", "missing"}};
    const auto required_result = compile(required_artifact);
    check(required_result.definition && program(*required_result.definition).expressions.at("/artifacts/main/required_when").evaluate({}, {}) == Json(true), "ArtifactDefaultIsRequired");
    const auto rejected = [&](const std::string& name, const std::string& expected, const std::function<void(Json&)>& change) {
        auto document = fixture();
        change(document["merge"]);
        const auto authority = authorize(document);
        const auto original = authority->definition().declaration;
        ExecutionCompileResult checked;
        try { checked = compile_execution_definition(authority); }
        catch (const std::exception&) { check(false, name); return; }
        check(code(checked, expected), name);
        check(authority->definition().declaration == original, name + ":AuthoredUnchanged");
    };
    rejected("BareLaunchRootRejected", "unsupported_execution_feature", [&](Json& d) { d["operations"][0]["request"]["set"] = {{"/x", {{"ref", "/launch"}}}}; });
    rejected("UnknownOperationRejected", "unknown_operation", [&](Json& d) { d["operations"][0]["operation"] = "unknown"; });
    rejected("UnknownModelModeRejected", "invalid_model_type", [&](Json& d) { d["operations"][0]["model_modes"] = {"unknown"}; });
    rejected("ModelModeUsesChatName", "invalid_model_type", [&](Json& d) { d["operations"][0]["model_modes"] = {"llm"}; });
    rejected("OptionBoundsReversed", "invalid_option_contract", [&](Json& d) { d["metadata"]["options"] = Json::array({option("ctx", "ctx", nullptr, "integer")}); d["metadata"]["options"][0]["minimum"] = 3; d["metadata"]["options"][0]["maximum"] = 2; });
    rejected("OptionEnumTypeRejected", "invalid_option_contract", [&](Json& d) { d["metadata"]["options"] = Json::array({option()}); d["metadata"]["options"][0]["enum"] = {"cpu", 2}; });
    rejected("OptionEnumBoundsRejected", "invalid_option_contract", [&](Json& d) { d["metadata"]["options"] = Json::array({option("ctx", "ctx", 2, "integer")}); d["metadata"]["options"][0]["enum"] = {2, 3}; d["metadata"]["options"][0]["maximum"] = 2; });
    rejected("BackendSlotRequiresString", "invalid_option_binding", [&](Json& d) { d["metadata"]["options"] = Json::array({option("ctx", "ctx", 2, "integer")}); d["launch"]["backend"] = {{"option", "ctx"}, {"fallback", "first_supported"}, {"on_empty", {{"operation", "run"}, {"target", "backend"}}}}; });
    rejected("PortStrategyFieldsRejected", "invalid_port_strategy", [&](Json& d) { d["launch"]["ports"][0]["span"] = 2; });
    rejected("OffsetPortRequiresProbedBase", "invalid_port_strategy", [&](Json& d) { d["launch"]["ports"].push_back({{"name", "offset"}, {"strategy", "offset"}, {"base", "http"}, {"offset", 1}, {"reject_zero", false}, {"error", ""}}); });
    rejected("ArtifactNeedsMissingError", "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}}; });
    rejected("ArtifactSentinelConfined", "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", ""}, {"cache_resolution", "sentinel_file"}, {"sentinel", "../file"}, {"selection", "first_existing"}}; });
    rejected("ArtifactFilesFromOwnsSelection", "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", ""}, {"files_from", "variant_directory"}}; });
    rejected("ArtifactFilesErrorNeedsSource", "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", ""}, {"files_error", "bad"}}; });
    rejected("HookInstallStrategyUnsupported", "unsupported_execution_feature", [&](Json& d) { d["install"]["strategy"] = "hook"; });
    rejected("OperationFormatsUnsupported", "unsupported_execution_feature", [&](Json& d) { d["operations"][0]["formats"] = {"png"}; });
    rejected("DuplicateOperationModes", "operation_overlap", [&](Json& d) { d["operations"][0]["model_modes"] = {"*", "chat"}; });
    rejected("InvalidDeliveryRejected", "unsupported_execution_feature", [&](Json& d) { d["operations"][0]["operation"] = "embeddings"; d["operations"][0]["delivery"] = "streaming"; });
    rejected("TransportMethodRejected", "unsupported_execution_feature", [&](Json& d) { d["operations"][0]["transport"]["method"] = "GET"; });
    rejected("UnknownTransportPortRejected", "invalid_reference", [&](Json& d) { d["operations"][0]["transport"]["port"] = "missing"; });
    rejected("EndpointMustBeRelative", "invalid_endpoint", [&](Json& d) { d["operations"][0]["transport"]["path"] = "https://PRIVATE_PATH"; });
    rejected("EndpointNeedsLeadingSlash", "invalid_endpoint", [&](Json& d) { d["operations"][0]["transport"]["path"] = "relative"; });
    rejected("EndpointCannotEmbedScheme", "invalid_endpoint", [&](Json& d) { d["operations"][0]["transport"]["path"] = "/http://PRIVATE_PATH"; });
    rejected("EndpointCannotContainNewline", "invalid_endpoint", [&](Json& d) { d["operations"][0]["transport"]["path"] = "/chat\r\n"; });
    rejected("EndpointCannotContainNul", "invalid_endpoint", [&](Json& d) { d["operations"][0]["transport"]["path"] = std::string("/chat\0", 6); });
    for (const auto& sentinel : std::vector<std::string>{"", ".", "..", "a/b", std::string("a\0b", 3)}) {
        rejected("ArtifactSentinel:" + std::to_string(sentinel.size()), "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", ""}, {"cache_resolution", "sentinel_file"}, {"sentinel", sentinel}, {"selection", "first_existing"}}; });
    }
    rejected("ArtifactSentinelSelection", "invalid_artifact", [&](Json& d) { d["artifacts"]["main"] = {{"role", "main"}, {"strategy", "resolved_path"}, {"missing_error", ""}, {"cache_resolution", "sentinel_file"}, {"sentinel", "model"}, {"selection", "unique"}}; });
    rejected("MappingRootDestinationRejected", "invalid_pointer", [&](Json& d) { d["operations"][0]["request"]["set"] = {{"", 1}}; });
    rejected("ReferenceRootRejected", "invalid_reference", [&](Json& d) { d["launch"]["argv"] = Json::array({{{"id", "raw"}, {"args", Json::array({{{"ref", ""}}})}}}); });
    rejected("ReferenceSyntaxRejected", "invalid_reference", [&](Json& d) { d["launch"]["argv"] = Json::array({{{"id", "raw"}, {"args", Json::array({{{"ref", "/model/~bad"}}})}}}); });
    rejected("ReferenceDefaultTypeRejected", "invalid_type", [&](Json& d) { d["operations"][0]["request"]["set"] = {{"/x", {{"ref", "/request/x"}, {"type", "boolean"}, {"default", 0}}}}; });
    rejected("PresentRequiresLookup", "invalid_expression", [&](Json& d) { d["operations"][0]["request"]["set"] = {{"/x", {{"present", 1}}}}; });
    rejected("RequestUnavailableDuringLaunch", "phase_dependency", [&](Json& d) { d["launch"]["argv"] = Json::array({{{"id", "raw"}, {"args", Json::array({{{"ref", "/request/x"}}})}}}); });
    value = fixture();
    value["merge"]["install"] = {{"strategy", "github_release"}};
    Json release = {{"id", "release"}, {"repo", "owner/repo"}};
    release["filename"]["concat"] = Json::array({"file-", {{"ref", "/install/version"}}, {{"ref", "/platform/os"}}});
    value["merge"]["install"]["cases"] = Json::array({release});
    const auto installed = compile(value);
    check(bool(installed.definition), "InstallFactsAndPlatformAvailableInInstallStage");
    check(installed.definition && program(*installed.definition).install.cases.size() == 1 && program(*installed.definition).install.cases[0].filename == "/install/cases/0/filename" && program(*installed.definition).install.cases[0].repo == "owner/repo", "InstallRecordsCompileToExpressionLocations");
    value["merge"]["install"]["cases"].push_back(value["merge"]["install"]["cases"][0]); value["merge"]["install"]["cases"][1]["id"] = "other";
    check(code(compile(value), "selector_overlap"), "InstallSelectorsDoNotOverlap");
    value["merge"]["install"]["cases"].erase(1); value["merge"]["install"]["supports_split_archive"] = true; value["merge"]["install"]["cases"][0]["sha256"] = std::string(64, 'a');
    check(code(compile(value), "unsupported_execution_feature"), "SplitArchiveDigestUnsupported");
    return finish();
}
