#include "backend_builtin_seeds_internal.h"
#include "backend_capabilities_generated.h"
#include "backend_catalog_execution_fixture.h"
#include "backend_catalog_publication_internal.h"
#include "lemon/router.h"
#include "llamacpp_oracle_descriptor.h"
#include "llamacpp_v2_oracle.h"

#include <fstream>
#include <iterator>

namespace lemon {
struct RoutingHelperTestHook {
    static std::unique_ptr<WrappedServer> create_backend(Router& router, const ModelInfo& model) {
        return router.create_backend_server(model);
    }
};
}

using namespace runtime_fixture;
namespace {
const DefinitionKey llama{DefinitionNamespace::Builtin, "llamacpp", 1};
const std::vector<std::string> recipes{"llamacpp", "llamacpp-hrx", "whispercpp", "moonshine", "kokoro", "sd-cpp",
    "flm", "ryzenai-llm", "vllm", "thenoise", "ds4", "cloud", "thinksound", "acestep", "onnxruntime", "trellis", "openmoss"};

void failed(const CatalogBuildResult& result, const std::string& name) {
    check(!result.catalog && !result.stages.executable && catalog_publication().state() == CatalogPublication::State::Empty,
        "FailedBuiltinOrDescendantPublishesNothing:" + name);
}

void authority_failures() {
    const auto& sources = embedded_v2_sources();
    auto missing = embedded_v2_policies();
    missing.leaves.clear();
    missing.digest = v2::export_registry_digest(missing.leaves, installed_hook_abis());
    const auto no_policy = BuiltinCatalogLoader::test_freeze_builtins(sources, missing);
    check(no_policy.snapshot && BuiltinCatalogLoader::builtin_leaves(*no_policy.snapshot).empty(), "MissingPolicyMintsNothing");
    if (no_policy.snapshot) failed(build_backend_catalog(CatalogAdmissionBuilder::test_builtins(no_policy.snapshot, sources), {}), "missing-policy");
    const std::vector<std::pair<std::string, std::function<void(BuiltinExecutionPolicies&)>>> mutations{
        {"source_digest_mismatch", [](auto& p) { p.leaves[0].expected_source_digest.back() ^= 1; }},
        {"definition_digest_mismatch", [](auto& p) { p.leaves[0].expected_definition_hash.back() ^= 1; }},
        {"dependency_digest_mismatch", [](auto& p) { p.leaves[0].expected_dependency_digest.back() ^= 1; }},
        {"policy_digest_mismatch", [](auto& p) { p.leaves[0].policy_digest.back() ^= 1; }},
        {"uncovered_hook_slot", [](auto& p) { p.leaves[0].slots.pop_back(); repin(p.leaves[0]); }},
        {"uncovered_operation_owner", [](auto& p) { p.leaves[0].owners.pop_back(); repin(p.leaves[0]); }}
    };
    for (const auto& [code, mutate] : mutations) {
        for (std::size_t leaf = 0; leaf < embedded_v2_policies().leaves.size(); ++leaf) {
            auto stale = embedded_v2_policies();
            std::swap(stale.leaves[0], stale.leaves[leaf]);
            mutate(stale);
            std::swap(stale.leaves[0], stale.leaves[leaf]);
            stale.digest = v2::export_registry_digest(stale.leaves, installed_hook_abis());
            const auto rejected = BuiltinCatalogLoader::test_freeze_builtins(sources, stale);
            check(!rejected.snapshot && has_code(rejected.diagnostics, code), "FixedBuiltinPolicyRejects:" + stale.leaves[leaf].key.id + ":" + code);
        }
    }
}

Json leaf_evidence(const ExecutableDefinition& executable, const BuiltinExportContract& policy) {
    const auto& token = executable.compiled().authorized();
    Json evidence{{"builtin_policy", v2::export_policy_json(policy)},
        {"source", token.definition().source_digest}, {"definition", token.definition().definition_hash},
        {"dependencies", token.definition().dependency_digest},
        {"authority", CatalogAdmissionBuilder::authority_identity(token)},
        {"compilation", executable.compiled().compilation_id()}, {"execution", executable.execution_id()},
        {"declaration", token.definition().declaration}};
    evidence["hooks"] = Json::array();
    evidence["grants"] = Json::array();
    const auto& program = ex::Inspection::program(executable.compiled());
    evidence["normalized"] = program.normalized;
    for (const auto& hook : program.hooks) {
        Json inputs = Json::array();
        for (const auto& input : hook.inputs) inputs.push_back({{"fact", input.index},
        {"producer", input.producer ? Json(*input.producer) : Json(nullptr)}});
        evidence["hooks"].push_back({{"grant", hook.grant_ordinal}, {"contract", static_cast<unsigned>(hook.contract)},
        {"owner_kind", static_cast<unsigned>(hook.slot.owner_kind)}, {"owner", hook.slot.owner_id},
        {"stage", hook.slot.stage}, {"id", hook.slot.hook_id}, {"order", hook.order},
        {"parameters", hook.parameter_digest}, {"owner_semantics", hook.owner_digest},
        {"policy", hook.policy_digest}, {"abi", hook.abi_revision}, {"implementation", hook.implementation_revision},
        {"effects", hook.effects}, {"inputs", inputs}, {"provides", hook.provides}, {"dependencies", hook.dependencies}});
    }
    for (std::size_t i = 0; i < AuthorityInspection::grant_count(token); ++i) {
        const auto& grant = AuthorityInspection::grant(token, i);
        evidence["grants"].push_back({{"builtin", grant.base_id}, {"contract", grant.contract_major}, {"slot", v2::slot_json(grant.slot)}});
    }
    return evidence;
}

void identity_evidence(const BackendCatalog& catalog, const AdmittedCatalog& admitted, const ExecutableDefinition& executable) {
    if (const auto* out = std::getenv("LEMON_SWITCH_OUT")) {
        const auto& policy = embedded_v2_policies().leaves.at(0);
        const auto& token = executable.compiled().authorized();
        Json arms = Json::array();
        for (const auto& row : catalog.registrations()) arms.push_back({row.descriptor->recipe,
            row.configured_entry && row.configured_entry->executable_definition ? "D2" : row.spec && row.spec->definition ? "D" : "A"});
        Json evidence{{"builtin_policy", v2::export_policy_json(policy)}, {"builtin_registry", embedded_v2_policies().digest},
            {"authority", CatalogAdmissionBuilder::authority_identity(token)}, {"admission", admitted.admission_id()},
            {"compilation", executable.compiled().compilation_id()}, {"execution", executable.execution_id()},
            {"catalog", catalog.export_schema().at("catalog_id")}, {"catalog_v2", catalog.export_schema("", 2).at("catalog_v2_id")},
            {"arms", arms}, {"declaration", token.definition().declaration}, {"exports", installed_builtin_exports().digest()},
            {"abi", v2::hook_abi_json(installed_hook_abis())}};
        evidence["descriptors"] = Json::array();
        evidence["spec_aliases"] = Json::array();
        evidence["v1_ids"] = Json::array();
        for (const auto& row : catalog.registrations()) {
            evidence["descriptors"].push_back(dump_llamacpp_descriptor(*row.descriptor));
            if (row.spec) evidence["spec_aliases"].push_back({row.spec->recipe, row.spec->pin_recipe});
            if (row.spec && row.spec->definition) evidence["v1_ids"].push_back(row.spec->definition->id);
        }
        evidence["common_defaults"] = RecipeOptions::common_option_defaults();
        evidence["hooks"] = Json::array();
        evidence["grants"] = Json::array();
        const auto& program = ex::Inspection::program(executable.compiled());
        evidence["normalized"] = program.normalized;
        for (const auto& hook : program.hooks) {
            Json inputs = Json::array();
            for (const auto& input : hook.inputs) inputs.push_back({{"fact", input.index},
                {"producer", input.producer ? Json(*input.producer) : Json(nullptr)}});
            evidence["hooks"].push_back({{"grant", hook.grant_ordinal}, {"contract", static_cast<unsigned>(hook.contract)},
                {"owner_kind", static_cast<unsigned>(hook.slot.owner_kind)}, {"owner", hook.slot.owner_id},
                {"stage", hook.slot.stage}, {"id", hook.slot.hook_id}, {"order", hook.order},
                {"parameters", hook.parameter_digest}, {"owner_semantics", hook.owner_digest},
                {"policy", hook.policy_digest}, {"abi", hook.abi_revision}, {"implementation", hook.implementation_revision},
                {"effects", hook.effects}, {"inputs", inputs}, {"provides", hook.provides}, {"dependencies", hook.dependencies}});
        }
        for (std::size_t i = 0; i < AuthorityInspection::grant_count(token); ++i) {
            const auto& grant = AuthorityInspection::grant(token, i);
            evidence["grants"].push_back({{"builtin", grant.base_id}, {"contract", grant.contract_major}, {"slot", v2::slot_json(grant.slot)}});
        }
        for (const auto& row : catalog.registrations()) if (row.descriptor->recipe == "llamacpp-hrx") {
            const auto* entry = row.configured_entry;
            check(entry && entry->executable_definition && !entry->definition && row.spec == &entry->spec &&
                row.ops == entry->backend_ops() && row.descriptor == &entry->published_descriptor(),
                "HrxCanonicalDescriptorSpecOpsFactoryOwnFinalizedExecutable");
            evidence["hrx"] = leaf_evidence(*entry->executable_definition, embedded_v2_policies().leaves.at(1));
        }
        std::ofstream(out) << evidence.dump(2) << '\n';
    }
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--assert-unavailable") {
        check(throws([] { (void)create_server("llamacpp", {"error"}); }), "FailedActivationFactoryThrows");
        bool refused = false;
        auto* previous = RuntimeConfig::global();
        try {
            RuntimeConfig config(Json{{"auto_evict", false}, {"inhibit_suspend", false}, {"log_level", "error"}, {"max_loaded_models", 4}});
            RuntimeConfig::set_global(&config);
            Router router(&config, nullptr, nullptr);
            ModelInfo unknown;
            unknown.recipe = "unregistered-failed-activation";
            (void)RoutingHelperTestHook::create_backend(router, unknown);
        } catch (const std::logic_error& error) {
            const std::string text = error.what();
            refused = text.find("authority") != std::string::npos || text.find("finalization") != std::string::npos;
        }
        RuntimeConfig::set_global(previous);
        check(refused, "FailedActivationRouterCannotSelectWrapper");
        return finish();
    }
    try {
        allow_entries = false;
        const auto& policy = embedded_v2_policies();
        check(policy.leaves.size() == 2 && policy.leaves[0].key.id == "llamacpp" && policy.leaves[0].key.contract == 1,
            "ExactlyTwoFixedBuiltinPolicies");
        if (policy.leaves.size() != 2) return finish();
        check(policy.leaves[0].slots.size() == 17 && policy.leaves[0].owners.size() == 11 &&
            policy.digest == v2::export_registry_digest(policy.leaves, installed_hook_abis()), "FixedPolicySeventeenOccurrencesElevenOwners");
        check(installed_builtin_exports().exports().empty() && installed_builtin_exports().digest() ==
            "sha256:b2e770dfdb68a27db72cbe2fdad29f26c3eeb7ab2ca5ea136d325bfe4a5dfa8a", "BuiltinPolicyDoesNotAuthorizeExport");
        std::ifstream file(std::string(LEMON_PRODUCTION_DEFINITIONS) + "/llamacpp.json", std::ios::binary);
        const std::string production_bytes{std::istreambuf_iterator<char>(file), {}};
        check(production_bytes == read_bytes("llamacpp.candidate.json") && embedded_v2_sources().size() == 2 &&
            std::any_of(embedded_v2_sources().begin(), embedded_v2_sources().end(), [&](const auto& source) { return source.key.id == "llamacpp" && source.bytes == production_bytes; }), "ProductionBytesEqualImmutableQualifiedCandidate");
        const auto nested = installed_definition_admission_features();
        check(nested.at("acceptance_stage") == "execution_finalization" &&
            nested.at("executable_definition_versions") == Json::array({1, 2}), "NestedAdmissionManifestReportsFinalizationAndVersions");
        authority_failures();
        const auto frozen = freeze_builtin_catalog();
        const auto snapshot = make_catalog_source_snapshot(frozen, {});
        const auto admitted = admit_catalog(snapshot.snapshot);
        check(admitted.catalog && admitted.catalog->metrics().definitions == 9, "ProductionSourcesSevenV1TwoV2ChargedOnce");
        const auto complete = build_backend_catalog(frozen, {});
        check(complete.catalog && complete.stages.executable, "ProductionGraphFinalizesPrivately");
        if (!complete.catalog || !admitted.catalog) { diagnostics(complete.diagnostics); return finish(); }
        check(catalog_publication().state() == CatalogPublication::State::Empty, "PrivateFinalizationPublishesNothing");
        auto seeds = builtin_registration_seeds();
        seeds.push_back(seeds.at(1));
        failed(build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(frozen, seeds), {}), "duplicate-recipe");
        seeds = builtin_registration_seeds();
        auto duplicate_descriptor = *seeds.front().descriptor;
        duplicate_descriptor.recipe = "duplicate-definition-fixture";
        auto duplicate_definition = seeds.front();
        duplicate_definition.descriptor = &duplicate_descriptor;
        seeds.push_back(duplicate_definition);
        failed(build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(frozen, seeds), {}), "duplicate-definition");
        seeds = builtin_registration_seeds();
        seeds.front().legacy = seeds.at(2).legacy;
        failed(build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(frozen, seeds), {}), "ambiguous-legacy-arm");
        seeds = builtin_registration_seeds();
        seeds.erase(seeds.begin());
        failed(build_backend_catalog(CatalogAdmissionBuilder::test_with_seeds(frozen, seeds), {}), "missing-canonical-seed");
        CatalogExecutionLimits limit;
        limit.compilation.normalized_bytes = 0;
        failed(build_backend_catalog(frozen, {}, limit), "compile");
        limit = {};
        limit.binding.slots = 0;
        failed(build_backend_catalog(frozen, {}, limit), "bind");
        limit = {};
        limit.stages[3].owned_bytes = 0;
        failed(build_backend_catalog(frozen, {}, limit), "entry");
        auto root = catalog_execution_fixture::executable_root();
        auto child = admission_fixture::child("broken-child", "catalog-root");
        child["merge"]["launch"]["readiness"]["path"] = "//remote/health";
        failed(catalog_execution_fixture::build({root, child}), "descendant");
        zero("ActualProductionValidation");
        const BackendRegistration* llama_row = nullptr;
        std::vector<std::string> actual_recipes;
        for (const auto& row : complete.catalog->registrations()) {
            actual_recipes.push_back(row.descriptor->recipe);
            if (row.descriptor->recipe == "llamacpp") llama_row = &row;
        }
        check(actual_recipes == recipes && llama_row && llama_row->configured_entry &&
            llama_row->configured_entry->executable_definition, "ExactlyOneLlamaRegistration");
        if (!llama_row || !llama_row->configured_entry) return finish();
        const auto& entry = *llama_row->configured_entry;
        check(llama_row->descriptor == &entry.published_descriptor() && llama_row->spec == &entry.spec &&
            llama_row->ops == entry.backend_ops() && entry.executable_definition == entry.spec.executable_definition &&
            !entry.definition && entry.bindings.size() == 11, "CanonicalDescriptorSpecOpsFactoryOwnFinalizedExecutable");
        {
            CatalogPublicationGuard guard(catalog_publication());
            for (const auto& read : std::vector<std::function<void()>>{
                [] { (void)all_registrations(); }, [] { (void)builtin_registrations(); }, [] { (void)all_descriptors(); },
                [] { (void)create_server("llamacpp", {"error"}); }})
                check(throws(read), "ReadsDuringBuildingFailWithoutFallback");
            guard.commit(complete.catalog->data(), complete.catalog);
        }
        allow_entries = true;
        auto server = llama_row->create({"error"});
        check(dynamic_cast<ConfiguredServer*>(server.get()) && server->get_descriptor() == llama_row->descriptor,
            "ActualFactoryCreatesCanonicalConfiguredServer");
        const auto op = BackendOperation::Embeddings, rank = BackendOperation::Reranking;
        const auto buffered = OperationDelivery::Buffered;
        for (const auto& capability : all_backend_capabilities()) {
            if (capability.descriptor->recipe == "llamacpp") {
                check(capability.capabilities == (CAP_EMBEDDINGS | CAP_RERANKING) &&
                    operation_adapter<IEmbeddingsServer>(*server, op, buffered, ModelType::LLM) &&
                    operation_adapter<IEmbeddingsServer>(*server, op, buffered, ModelType::EMBEDDING) &&
                    operation_adapter<IRerankingServer>(*server, rank, buffered, ModelType::LLM) &&
                    operation_adapter<IRerankingServer>(*server, rank, buffered, ModelType::RERANKING) &&
                    !operation_adapter<IClassificationServer>(*server, BackendOperation::Classification, buffered, ModelType::LLM),
                    "CapabilityMaskIndependentlyMatchesFinalizedBindingsAndTypedAdapters");
            }
            if (capability.descriptor->recipe == "llamacpp-hrx") check(capability.capabilities == 0, "HrxCapabilityRemainsZero");
        }
        const auto token = admitted.catalog->builtin_v2_definition(llama);
        check(token && &token->definition() && entry.executable_definition->compiled().authorized().definition().definition_hash ==
            token->definition().definition_hash, "FinalizedExecutableUsesInstalledBuiltinAuthority");
        const auto sidecar = llama_corpus::sidecar(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/llamacpp.deliberate-diffs.json");
        llama_corpus::validate_sidecar(sidecar);
        for (const auto& view : llama_corpus::views)
            llama_corpus::compare(view, llama_corpus::capture(view, entry.executable_definition), sidecar);
        identity_evidence(*complete.catalog, *admitted.catalog, *entry.executable_definition);
        check(&all_registrations() == &complete.catalog->registrations() && descriptor_for("llamacpp") == llama_row->descriptor &&
            spec_for("llamacpp") == llama_row->spec && ops_for("llamacpp") == llama_row->ops, "PublicationExposesOneCoherentGeneration");
        RuntimeConfig config(Json{{"auto_evict", false}, {"inhibit_suspend", false}, {"log_level", "error"}, {"max_loaded_models", 4}});
        auto* previous = RuntimeConfig::global();
        RuntimeConfig::set_global(&config);
        {
            Router router(&config, nullptr, nullptr);
            ModelInfo unknown;
            unknown.recipe = "unregistered-fallback-fixture";
            auto fallback = RoutingHelperTestHook::create_backend(router, unknown);
            check(dynamic_cast<ConfiguredServer*>(fallback.get()) && fallback->get_descriptor() == llama_row->descriptor &&
                fallback->supports_operation(op, buffered, ModelType::EMBEDDING), "NoLlamaWrapperFallback");
        }
        RuntimeConfig::set_global(previous);
    } catch (const std::exception& error) {
        check(false, "ProductionSwitch:" + std::string(error.what()));
    }
    return finish();
}
