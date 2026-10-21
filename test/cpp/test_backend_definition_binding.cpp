#include "backend_definition_binding_fixture.h"

#include "lemon/recipe_options.h"

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace binding_fixture;

int main() {
    try {
        auto value = fixture();
        zero("hook-bearing compilation");
        const auto ungranted = compile_execution_definition(AuthorityInspection::test_without_grants(*value.token));
        check(!ungranted.definition && has_code(ungranted.diagnostics, "unsupported_execution_feature"),
            "SealedSlotIsNotDeclarationLookup:declarations cannot manufacture missing grants");
        zero("ungranted hook-bearing compilation");
        const auto result = bind(value);
        zero("successful binding");
        check(bool(result.definition), "AllTenTargetKindsBind");
        if (!result.definition) { diagnostics(result.diagnostics); return finish(); }
        const auto& program = ex::Inspection::program(*value.compiled);
        std::set<typed::Contract> kinds;
        for (const auto& hook : program.hooks) kinds.insert(hook.contract);
        check(kinds.size() == 10 && bd::Inspection::hooks(*result.definition).size() == program.hooks.size(), "AllTenTargetKindsBoundExactlyOncePerGrant");
        check(program.expressions.at("/launch/argv/1/value").producer.has_value() &&
            program.expressions.at("/launch/argv/1/value").fact_field == 6 &&
            program.expressions.at("/launch/argv/1/value").type == "integer", "NamedTypedProducerFactBoundWithoutInvocation");
        reject(changed(value, [](auto& p) { p.expressions.at("/launch/argv/1/value").producer = 0; }),
            "sealed_dependency_mismatch", "SealedSlotIsNotDeclarationLookup:expression producer");
        const auto expected = Json::parse(read_bytes("binding-chains.json"));
        check(program.operations.size() == 11 && expected.size() == 11, "ElevenOperationCellsIncludingEmptyChain");
        for (const auto& cell : expected) {
            Json request = Json::array(), response = Json::array();
            for (const auto& hook : program.hooks) if (hook.slot.owner_kind == SlotOwnerKind::Operation && hook.slot.owner_id == cell.at("id")) {
                if (hook.slot.stage == "request") request.push_back(hook.slot.hook_id);
                if (hook.slot.stage == "response") response.push_back(hook.slot.hook_id);
            }
            check(request == cell.at("request") && response == cell.at("response"), "IndependentOperationChain:" + cell.at("id").get<std::string>());
        }
        for (const auto& attack : std::vector<std::pair<std::string, std::function<void(Json&)>>>{
            {"unsupported_execution_feature", [](auto& d) { d["merge"]["launch"]["argv"][1]["value"] = {{"ref", "/launch/hook/arbitrary"}}; }},
            {"unsupported_execution_feature", [](auto& d) { d["merge"]["launch"]["argv"][1]["value"] = {{"ref", "/launch/hook"}}; }},
            {"phase_dependency", [](auto& d) { d["merge"]["hooks"].erase("launch"); d["merge"]["launch"]["prepare_order"] = {"backend", "artifacts", "program", "ports"}; d["merge"]["launch"]["argv"][1]["value"] = 2048; d["merge"]["operations"][0]["request"]["set"] = {{"/fact", {{"ref", "/launch/hook/context_size"}}}}; }},
            {"invalid_type", [](auto& d) { d["merge"]["launch"]["argv"][1]["value"]["type"] = "boolean"; }},
            {"phase_dependency", [](auto& d) { d["merge"]["launch"]["custom_args"]["defaults"][0]["stage"] = "runtime_options"; d["merge"]["launch"]["custom_args"]["defaults"][0]["when"] = {{"ref", "/launch/hook/hf_load"}}; }},
            {"unsupported_execution_feature", [](auto& d) { d["merge"]["artifacts"]["main"]["hook"]["id"] = "llamacpp.gguf"; }}}) {
            auto doc = document();
            attack.second(doc);
            const auto admitted = admit({}, builtins(false, &doc));
            check(bool(admitted.catalog), "HookBearingFailureFixtureAdmitted");
            if (!admitted.catalog) { diagnostics(admitted.diagnostics); continue; }
            const auto rejected = compile_execution_definition(admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "binding-fixture", 1}));
            check(!rejected.definition && has_code(rejected.diagnostics, attack.first), "HookBearingCompilerFailsClosed:" + attack.first);
            zero("hook-bearing compilation failure");
        }
        const auto standalone_bytes = root().dump();
        const auto standalone = DefinitionAuthorizer(load_v2_authority_snapshot({
            {{DefinitionNamespace::External, "root", 0}, standalone_bytes, ""}}).snapshot)
            .authorize({DefinitionNamespace::External, "root", 0});
        reject({value.admitted, standalone.definition, compile_execution_definition(standalone.definition).definition},
            "token_not_admitted", "ExactCompiledTokenBelongsToTransaction:public standalone authorizer");
        auto other = fixture();
        check(value.admitted->admission_id() == other.admitted->admission_id() && value.compiled->compilation_id() == other.compiled->compilation_id(), "EqualSemanticDigestsHaveSeparateMembership");
        bd::counters = {};
        reject({other.admitted, value.token, value.compiled}, "token_not_admitted", "ExactCompiledTokenBelongsToTransaction:byte-identical B");
        check(bd::counters.target_checks == 0, "MembershipPrecedesTargetCorrespondence");
        const auto clone = CatalogAdmissionBuilder::test_clone_token(*value.token);
        const auto cloned = compile_execution_definition(clone);
        check(bool(cloned.definition), "MatchingTicketCloneStillCompilesSymbolically");
        reject({value.admitted, clone, cloned.definition}, "token_not_admitted", "ExactCompiledTokenBelongsToTransaction:matching-ticket clone");
        const auto detached = CatalogAdmissionBuilder::test_clone_token(*value.token);
        CatalogAdmissionBuilder::test_detach_ticket(*detached);
        reject({value.admitted, detached, compile_execution_definition(detached).definition}, "token_not_admitted", "ExactCompiledTokenBelongsToTransaction:standalone hook token");
        auto listed = fixture();
        CatalogAdmissionBuilder::test_detach_ticket(*listed.token);
        reject(listed, "token_not_admitted", "ExactCompiledTokenBelongsToTransaction:listed detached ticket");
        const auto artifact_slot = static_cast<std::size_t>(std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) {
            return hook.contract == typed::Contract::Artifacts;
        }) - program.hooks.begin());
        const auto attack = [&](const std::string& name, const std::function<void(ex::SymbolicHook&)>& change,
            const std::string& code = "sealed_slot_mismatch", std::size_t index = 0) {
            reject(changed(value, [&](auto& p) { change(p.hooks.at(index)); }), code, "SealedSlotIsNotDeclarationLookup:" + name);
        };
        attack("owner", [](auto& h) { h.slot.owner_id = "forged"; });
        attack("owner kind", [](auto& h) { h.slot.owner_kind = SlotOwnerKind::Operation; }, "sealed_slot_mismatch", artifact_slot);
        attack("stage", [](auto& h) { h.slot.stage = "runtime_options"; });
        attack("order", [](auto& h) { ++h.order; });
        attack("parameters", [](auto& h) { h.parameter_digest.back() ^= 1; });
        attack("ABI", [](auto& h) { ++h.abi_revision; });
        attack("implementation", [](auto& h) { ++h.implementation_revision; });
        attack("effects", [](auto& h) { h.effects.push_back(typed::Effect::SystemProbe); });
        attack("contract", [](auto& h) { h.contract = typed::Contract::RuntimeOptions; });
        attack("target ID", [](auto& h) { h.slot.hook_id = "llamacpp.runtime_options.v1"; });
        attack("owner semantic identity", [](auto& h) { h.owner_digest.back() ^= 1; });
        attack("policy identity", [](auto& h) { h.policy_digest.back() ^= 1; });
        attack("grant ordinal", [](auto& h) { ++h.grant_ordinal; });
        attack("input type", [](auto& h) { h.inputs.front().fact = typed::Fact::SystemFacts; }, "sealed_dependency_mismatch");
        attack("input index", [](auto& h) { ++h.inputs.front().index; }, "sealed_dependency_mismatch");
        attack("producer identity", [&](auto& h) { h.inputs.front().producer = artifact_slot; }, "sealed_dependency_mismatch");
        attack("provided fact", [](auto& h) { h.provides.push_back(typed::Fact::SystemFacts); });
        attack("dependency", [&](auto& h) { h.dependencies.push_back(artifact_slot); }, "sealed_dependency_mismatch");
        reject(changed(value, [](auto& p) { p.hooks.pop_back(); }), "sealed_slot_mismatch", "SealedSlotIsNotDeclarationLookup:missing grant");
        auto targets = typed::installed_targets();
        for (const auto& mutation : std::vector<std::function<void(typed::Registration&)>>{
            [](auto& t) { ++t.abi_revision; }, [](auto& t) { ++t.implementation_revision; },
            [](auto& t) { t.effects.clear(); }, [](auto& t) { t.requires.clear(); },
            [](auto& t) { t.provides.clear(); }, [](auto& t) { std::get<typed::ArtifactOps>(t.target).metadata = nullptr; }}) {
            auto copy = targets;
            mutation(copy.front());
            bd::test_targets = &copy;
            reject(value, "sealed_target_mismatch", "SealedSlotIsNotDeclarationLookup:installed target corruption");
            bd::test_targets = nullptr;
        }
        check(bind(other).definition->execution_id() == result.definition->execution_id(), "ExecutionIdentityUsesSemanticsNeverTicketOrAddress");
        bd::Budget identity_budget{{}, {}};
        check(bd::execution_identity(*other.admitted, *other.compiled, identity_budget, bd::binder_revision + 1) != result.definition->execution_id(),
            "BinderRevisionChangesSemanticExecutionIdentity");
        auto hrx = fixture(true);
        const auto artifact_bound = bind(hrx);
        check(bool(artifact_bound.definition), "ArtifactOnlyHrxHasNoVanillaCapabilities:binds");
        if (!artifact_bound.definition) return finish();
        const auto& hrx_program = ex::Inspection::program(*hrx.compiled);
        unsigned artifact_slots = 0;
        for (const auto& hook : hrx_program.hooks) {
            if (hook.contract == typed::Contract::Artifacts) ++artifact_slots;
            check(hook.contract != typed::Contract::RuntimeOptions && hook.contract != typed::Contract::Prepare &&
                hook.contract != typed::Contract::Environment && hook.contract != typed::Contract::System,
                "ArtifactOnlyHrxHasNoVanillaCapabilities:slot");
            for (const auto& input : hook.inputs) check(input.fact != typed::Fact::RuntimeFacts && input.fact != typed::Fact::ModelFacts &&
                input.fact != typed::Fact::EnvironmentPlan && input.fact != typed::Fact::SystemFacts,
                "ArtifactOnlyHrxHasNoVanillaCapabilities:dependent fact");
        }
        check(artifact_slots == 1, "ArtifactOnlyHrxHasNoVanillaCapabilities:artifact slot retained");
        zero("HRX compile and bind before positive invocation");
        lemon::RecipeOptions runtime_options("llamacpp", {{"ctx_size", 2048}, {"llamacpp_args", "--parallel 7"}});
        const auto options_before = runtime_options.to_json();
        const auto defaults_before = lemon::RecipeOptions::common_option_defaults();
        const auto environment = [] { const auto* path = std::getenv("PATH"); return path ? std::string(path) : std::string(); };
        const auto environment_before = environment();
        const Json runtime = {{"dflash", false}, {"mtp", false}, {"parallel", 7}, {"context", 2048}, {"environment", {{"PATH", "unchanged"}}}};
        auto state = runtime;
        const auto normalized = hrx.compiled->normalized_declaration();
        EffectSentinels effects;
        (void)effects;
        ConfinedReads reads;
        allow_entries = true;
        for (const auto& bound : bd::Inspection::hooks(*artifact_bound.definition)) {
            if (hrx_program.hooks.at(bound.symbolic_index).contract != typed::Contract::Artifacts) continue;
            const auto& ops = std::get<typed::ArtifactOps>(bound.target);
            const auto metadata = ops.metadata({reads.file(), "model.gguf", lemon::ModelType::LLM, {"existing"}}, reads);
            check(metadata.header && metadata.header->architecture == "qwen3" && metadata.header->context_length == 32768 &&
                metadata.labels == std::vector<std::string>({"existing", "vision", "tool-calling", "mtp"}), "ArtifactOnlyHrxHasNoVanillaCapabilities:real bound metadata");
            check(ops.validate_registration("repo/model-GGUF").find("CHECKPOINT:VARIANT") != std::string::npos &&
                ops.validate_registration("repo/model-GGUF:Q4_K_M").empty(), "ArtifactOnlyHrxHasNoVanillaCapabilities:real bound registration");
            check(ops.validate_file({reads.file(), "model.gguf"}, reads).empty(), "ArtifactOnlyHrxHasNoVanillaCapabilities:real bound file validation");
            check(ops.resolve_path({reads.root(), "main", "Q4_K_M"}, reads) == "cache/model-Q4_K_M.gguf", "ArtifactOnlyHrxHasNoVanillaCapabilities:real bound resolution");
            check(ops.find_imported({reads.root()}, reads) == "cache/model-Q4_K_M.gguf", "ArtifactOnlyHrxHasNoVanillaCapabilities:real bound import");
        }
        check(calls.size() == 5 && reads.reads == 4 && service_calls == 0 && state == runtime &&
            hrx.compiled->normalized_declaration() == normalized && runtime_options.to_json() == options_before &&
            runtime_options.get_option("ctx_size") == 2048 && lemon::RecipeOptions::common_option_defaults() == defaults_before &&
            environment() == environment_before, "ArtifactOnlyHrxHasNoVanillaCapabilities:runtime defaults context environment unchanged, zero runtime/system sentinels");
        calls.clear(); allow_entries = false;
        std::weak_ptr<const AdmittedCatalog> weak_catalog = value.admitted;
        std::weak_ptr<const CompiledDefinition> weak_compiled = value.compiled;
        const auto* original_token = value.token.get();
        const auto* original_compiled = value.compiled.get();
        value = {};
        check(!weak_catalog.expired() && !weak_compiled.expired() && &result.definition->compiled() == original_compiled &&
            &result.definition->compiled().authorized() == original_token, "BoundObjectRetainsExactInputs");
    } catch (const std::exception& error) { check(false, std::string("HookBearingCompileAndBindInvokeNothing:unexpected ") + error.what()); }
    return finish();
}
