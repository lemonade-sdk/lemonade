#include "backend_catalog_execution_fixture.h"
#include "backend_catalog_publication_internal.h"
#include "backend_runtime_fixture.h"
#include "lemon/backends/configured/backend_definition_descriptor.h"

using namespace catalog_execution_fixture;
using namespace lemon;

int main() {
    auto parent = executable_root();
    auto leaf = child("catalog-leaf", "catalog-root");
    auto first_server = builtin_registrations().front().create({"error"});
    const auto first_instance = first_server->get_instance_id();
    const auto good = build({parent, leaf, legacy()});
    auto next_server = builtin_registrations().front().create({"error"});
    check(next_server->get_instance_id() == first_instance + 1, "CatalogValidationConstructsNoRuntimeServers");
    check(good.catalog && good.stages.resolved && good.stages.authorized && good.stages.catalog_admitted &&
        good.stages.compiled && good.stages.bound && good.stages.executable, "WholeMixedGraphFinalizes");
    if (!good.catalog) { dump_errors(good); return finish(); }
    check(good.catalog->registrations().size() == 20 && !backend_catalog(), "PrivateBuildNeverPublishes");
    const auto before = good.catalog->export_schema();
    auto changed = parent;
    changed["merge"]["metadata"]["display_name"] = "Changed parent";
    const auto candidate = good.catalog->validate(changed.dump());
    check(candidate["valid"] == true && candidate["stages"]["compiled"] == true && candidate["stages"]["bound"] == true &&
        candidate["stages"]["executable"] == true, "CandidateFinalizesAllDescendants");
    check(good.catalog->export_schema() == before && !backend_catalog(), "CandidateKeepsActiveGraphAndPublicationUnchanged");
    auto invalid_compile = leaf;
    invalid_compile["merge"]["operations"] = parent["merge"]["operations"];
    invalid_compile["merge"]["operations"][0]["transport"]["kind"] = "http_bytes";
    const auto compiled_failure = build({parent, invalid_compile});
    if (compiled_failure.catalog) std::puts("descendant unexpectedly compiled");
    check(!compiled_failure.catalog && compiled_failure.stages.catalog_admitted && !compiled_failure.stages.compiled &&
        has_code(compiled_failure.diagnostics, "unsupported_execution_feature") && !backend_catalog(), "FailingDescendantCompilationRejectsWholeProposal");
    auto larger_leaf = leaf;
    larger_leaf["merge"]["launch"]["argv"] = Json::array();
    for (unsigned i = 0; i < 128; ++i) larger_leaf["merge"]["launch"]["argv"].push_back({{"id", "extra-" + std::to_string(i)}, {"args", {std::string(2048, 'x')}}});
    const auto binding_graph = build({parent, larger_leaf});
    check(bool(binding_graph.catalog), "DescendantBindingFixtureFinalizesBeforeReducingLimits");
    if (!binding_graph.catalog) { dump_errors(binding_graph); return finish(); }
    CatalogExecutionLimits binding_limit;
    binding_limit.binding.retained_bytes = 0;
    for (const auto& row : binding_graph.catalog->registrations()) {
        if (row.descriptor->recipe == "catalog-leaf") continue;
        if (row.spec && row.spec->executable_definition)
            binding_limit.binding.retained_bytes = std::max(binding_limit.binding.retained_bytes,
                row.spec->executable_definition->metrics().retained_bytes);
    }
    check(binding_graph.catalog->registrations().back().spec->executable_definition->metrics().retained_bytes > binding_limit.binding.retained_bytes,
        "BindingFailureFixtureExceedsEveryPrecedingEntry");
    binding::counters = {};
    const auto binding_failure = build({parent, larger_leaf}, binding_limit);
    check(!binding_failure.catalog && binding_failure.stages.compiled && !binding_failure.stages.bound &&
        binding::counters.membership_scans == 2 && has_code(binding_failure.diagnostics, "limit_exceeded") && !backend_catalog(),
        "FailingDescendantBindingRejectsAlreadyBoundParent");
    auto invalid_adapter = leaf;
    invalid_adapter["merge"]["launch"]["readiness"]["path"] = "//remote/health";
    invalid_adapter["merge"]["launch"]["watchdog"]["path"] = "//remote/health";
    const auto entry_failure = build({parent, invalid_adapter});

    check(!entry_failure.catalog && entry_failure.stages.compiled && entry_failure.stages.bound &&
        !entry_failure.stages.executable && has_code(entry_failure.diagnostics, "invalid_binding") && !backend_catalog(),
        "FailingDescendantFinalizationCannotEscapeOrPublishPrefix");
    auto constrained_leaf = leaf;
    constrained_leaf["merge"]["operations"] = parent["merge"]["operations"];
    const auto active_with_constraint = build({parent, constrained_leaf});
    changed = parent;
    changed["merge"]["launch"]["ports"] = Json::array({{{"name", "other"}, {"reject_zero", false}, {"error", ""}}});
    changed["merge"]["operations"][0]["transport"]["port"] = "other";
    changed["merge"]["launch"]["readiness"]["port"] = "other";
    changed["merge"]["launch"]["watchdog"]["port"] = "other";
    check(bool(build({changed}).catalog), "ChangedParentAloneStillFinalizes");
    const auto invalid_candidate = active_with_constraint.catalog->validate(changed.dump());
    check(invalid_candidate["valid"] == false && !backend_catalog(), "CandidateRebuildsDependencyGraphInsteadOfReusingDescendants");
    auto managed = parent;
    managed["merge"]["install"] = {{"strategy", "github_release"}, {"cases", Json::array({{{"id", "cpu"}, {"backend", {"cpu"}}, {"repo", "fake/engine"}, {"filename", "fake.zip"}}})}};
    const auto missing_pin = build({managed});
    check(!missing_pin.catalog && missing_pin.stages.bound && has_code(missing_pin.diagnostics, "required"), "V2ManagedVersionsCheckedBeforePublication");
    managed["merge"]["install"]["default_versions"] = {{"cpu", "v1"}};
    check(bool(build({managed}).catalog), "V2ManagedVersionCoverageCanSucceed");
    auto foreign = parent;
    foreign["merge"]["hooks"]["backend"] = {{"id", "llamacpp.backend"}};
    check(!build({foreign}).catalog, "ExternalDeclarationsCannotMintHookAuthority");
    check(installed_builtin_exports().exports().empty() && embedded_v2_sources().size() == 1 &&
        embedded_v2_sources()[0].key.id == "llamacpp", "ProductionV2LlamaOnlyAndExportsStayEmpty");
    const auto rich = good.catalog->export_schema("catalog-leaf", 2);
    check(rich["recipes"][0]["input_schema_version"] == 2 && rich["executable_definition_versions"] == Json::array({1, 2}), "ExecutableV2PublicProjectionUsesLeafVersion");
    check(before["catalog_id"] == build({parent, leaf, legacy()}).catalog->export_schema()["catalog_id"], "ExecutionCatalogIdentityIsDeterministic");

    binding_fixture::calls.clear();
    binding_fixture::allow_entries = false;
    const auto builtin = runtime_fixture::runtime_document();
    const auto trusted_builtins = binding_fixture::builtins(false, &builtin);
    const auto trusted_admission = admit({}, trusted_builtins);
    const auto trusted_token = trusted_admission.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "binding-fixture", 1});
    const auto trusted_descriptor = descriptor_from_declaration(trusted_token->definition().declaration,
        DefinitionNamespace::Builtin, DescriptorPlatform::native());
    auto trusted_seeds = builtin_registration_seeds();
    trusted_seeds.push_back({&trusted_descriptor, "binding-fixture", {}});
    check(!build({}, {}, CatalogAdmissionBuilder::test_with_seeds(trusted_builtins, trusted_seeds)).catalog,
        "SyntheticAuthorityCannotServeProductionLlamaSeed");
    trusted_seeds.erase(std::remove_if(trusted_seeds.begin(), trusted_seeds.end(), [](const auto& seed) { return seed.definition_id == "llamacpp"; }), trusted_seeds.end());
    const auto trusted = build({}, {}, CatalogAdmissionBuilder::test_with_seeds(trusted_builtins, trusted_seeds));
    check(trusted.catalog && trusted.stages.executable && binding_fixture::calls.empty(), "HookBearingWholeGraphFinalizationInvokesNothing");
    if (!trusted.catalog) dump_errors(trusted);

    runtime_fixture::Upstream upstream;
    auto runnable = executable_root("catalog-runnable");
    runnable["merge"]["launch"]["watchdog"]["policy"] = "none";
    runnable["merge"]["operations"][0]["transport"]["path"] = "/fake/catalog";
    runnable["merge"]["operations"][0]["request"]["set"] = {{"/mapped", 73}};
    const auto running = build({runnable});
    check(bool(running.catalog), "FinalizedEntryCanBeSelectedForRuntime");
    if (running.catalog) {
        const auto& registration = running.catalog->registrations().back();
        auto factory_server = registration.create({"error"});
        factory_server->set_descriptor(registration.descriptor);
        auto* configured = dynamic_cast<ConfiguredServer*>(factory_server.get());
        check(configured && registration.spec->executable_definition && registration.ops, "FinalizedFactorySpecOpsAndDescriptorMatch");
        const auto entry = ConfiguredBackendEntry::make(registration.spec->executable_definition);
        auto launch = std::make_shared<runtime_fixture::Launch>();
        launch->port = upstream.port;
        ServerCommand command;
        ConfiguredServer server(entry, {"error"}, {launch, [&](auto, const auto&) { return std::make_unique<runtime_fixture::Process>(command); }});
        ModelInfo model; model.model_name = "fake"; model.recipe = "catalog-runnable";
        RecipeOptions options;
        options.set_option("catalog-runnable_backend", "");
        server.set_model_metadata("fake", "fixture", ModelType::LLM, DEVICE_CPU, options);
        server.load("fake", model, options);
        server.set_state(ModelState::READY);
        check(server.chat_completion({{"marker", 6}})["mapped"] == 73, "PublishedEligibleExternalV2RunsThroughFakeEngineAndLoopback");
        CatalogPublication publication;
        { CatalogPublicationGuard guard(publication); guard.commit(running.catalog->data(), running.catalog); }
        const auto instance = server.get_instance_id();
        auto pending = std::async(std::launch::async, [&] { return server.chat_completion({{"marker", 81}, {"hold", true}}); });
        {
            std::unique_lock<std::mutex> lock(upstream.mutex);
            check(upstream.condition.wait_for(lock, std::chrono::seconds(3), [&] { return upstream.entered != 0; }),
                "PublishedRequestReachesUpstreamBeforeReplacementAttempt");
        }
        bool replacement_rejected = false;
        try { CatalogPublicationGuard replacement(publication); } catch (const std::logic_error&) { replacement_rejected = true; }
        auto update = runnable;
        update["merge"]["metadata"]["display_name"] = "Private pending candidate";
        const auto private_update = running.catalog->validate(update.dump());
        check(replacement_rejected && private_update["valid"] == true && publication.consume_server() == running.catalog.get() &&
            publication.data() == running.catalog->data().get(), "RejectedReplacementAndCandidatePreserveInFlightGeneration");
        {
            std::lock_guard<std::mutex> lock(upstream.mutex);
            upstream.release = true; upstream.condition.notify_all();
        }
        check(pending.get()["marker"] == 81 && server.get_instance_id() == instance &&
            server.chat_completion({{"marker", 82}})["marker"] == 82, "InFlightAndSubsequentRequestsSurviveRejectedReplacement");
        server.unload();
    }
    return finish();
}
