#include "backend_builtin_source_resources.h"
#include "backend_builtin_source_test_descriptor.h"
#include "backend_catalog_publication_internal.h"
#include "backend_definition_resources.h"
#include "lemon/backends/configured/backend_definition_resolver.h"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

#ifndef LEMON_SOURCE_CACHE_DATA_ONLY
#include "backend_builtin_entry_internal.h"
#include "backend_builtin_seeds_internal.h"
#include "backend_definition_exports_internal.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/wrapped_server.h"
#endif

const lemon::BackendDescriptor* source_cache_descriptor_peer();
const lemon::backends::ResolvedDefinition* source_cache_resolved_peer();

using namespace lemon;
using namespace lemon::backends;
namespace {
int checks = 0, failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", name); }
}
const DefinitionKey key{DefinitionNamespace::Builtin, "source-cache-fixture", 1};
}

#ifdef LEMON_SOURCE_CACHE_DATA_ONLY
#include "lemon/backends/backend_registry.h"
namespace {
unsigned registrations_entered = 0;
}
namespace lemon::backends {
const BackendDescriptor* descriptor_for(const std::string&) {
    catalog_publication().data();
    return nullptr;
}
const std::vector<BackendRegistration>& builtin_registrations() {
    ++registrations_entered;
    static const std::vector<BackendRegistration> empty;
    return empty;
}
}
#else
namespace lemon::backends {
const std::vector<DefinitionSourceView>& embedded_v2_sources() { return builtin_source_views(); }
const BuiltinExecutionPolicies& embedded_v2_policies() {
    static const auto policies = [] {
        const auto& resolved = builtin_resolved_source(key);
        BuiltinExportContract policy{{key.id, 1}, resolved.source_digest, resolved.definition_hash,
            resolved.dependency_digest, 1, "", {}};
        const DefinitionLimits limits;
        std::size_t retained = 0;
        v2::Budget budget{limits, {}, retained};
        for (const auto& operation : resolved.declaration.at("operations"))
            policy.owners.push_back({operation.at("id"), v2::operation_owner_semantics(operation, budget),
                {}, {}, {}, false, {}, v2::operation_owner_fields(), {}});
        policy.owner_graph_digest = v2::owner_graph_digest(policy.owners);
        policy.policy_digest = v2::export_policy_digest(policy);
        auto result = fixed_builtin_v2_policies();
        result.leaves.push_back(std::move(policy));
        result.digest = v2::export_registry_digest(result.leaves, installed_hook_abis());
        return result;
    }();
    return policies;
}
}
#endif

int run_test(int argc, char** argv) {
    const std::string order = argc > 1 ? argv[1] : "source";
    check(catalog_publication().state() == CatalogPublication::State::Empty, "NoPublicationConsumptionDuringDescriptorInit");
#ifdef LEMON_SOURCE_CACHE_DATA_ONLY
    check(registrations_entered == 0, "NoRegistrationDuringDescriptorInit");
#else
    const auto authority = BuiltinCatalogLoader::freeze_embedded();
    if (!authority.snapshot) {
        for (const auto& error : authority.diagnostics.entries) std::printf("AUTH %s %s\n", error.code.c_str(), error.pointer.c_str());
        return 1;
    }
    if (order == "freeze") check(bool(freeze_builtin_catalog()), "FreezeFirstAccess");
    if (order == "seeds") check(builtin_registration_seeds().size() == 18, "SeedsFirstAccess");
    if (order == "registrations" || order == "public-fallback") {
        const auto& registrations = order == "registrations" ? builtin_registrations() : all_registrations();
        check(registrations.size() == 18 && registrations.back().descriptor == &source_cache_fixture::descriptor &&
            registrations.back().create && registrations.back().spec && registrations.back().ops,
            "BuiltinOnlyFallbackIsCompletelyFinalized");
        if (order == "public-fallback") {
            check(catalog_publication().state() == CatalogPublication::State::BuiltinsConsumed, "PublicFallbackConsumesPublication");
            bool rejected = false;
            try { CatalogPublicationGuard guard(catalog_publication()); } catch (const std::logic_error&) { rejected = true; }
            check(rejected, "FallbackRejectsLatePublication");
            std::printf("%d checks, %d failures (%s)\n", checks, failures, order.c_str());
            return failures ? 1 : 0;
        }
    }
#endif
    if (order == "peer") check(source_cache_descriptor_peer()->recipe == key.id, "PeerFirstAccess");
    const auto& resolved = builtin_resolved_source(key);
    check(&resolved == &builtin_resolved_source(key) && &resolved == source_cache_resolved_peer(), "ResolvedSourceRetainedAcrossTranslationUnits");
    const auto snapshot = make_v2_source_snapshot(builtin_source_views());
    DefinitionResolver resolver(snapshot.snapshot);
    const auto control = resolver.resolve(key);
    check(control.definition && control.definition->source_digest == resolved.source_digest &&
        control.definition->definition_hash == resolved.definition_hash && control.definition->declaration == resolved.declaration,
        "ResolvedCacheUsesEmbeddedBytes");
    check(source_cache_descriptor_peer() == &source_cache_fixture::descriptor, "SameDescriptorAddressAcrossTranslationUnits");
    for (const auto& wrong : {DefinitionKey{DefinitionNamespace::External, key.id, 0},
        DefinitionKey{DefinitionNamespace::External, key.id, 1},
        DefinitionKey{DefinitionNamespace::Builtin, key.id, 2}, DefinitionKey{DefinitionNamespace::Builtin, "missing", 1}}) {
        bool rejected = false;
        try { (void)builtin_resolved_source(wrong); } catch (const std::logic_error&) { rejected = true; }
        check(rejected, "ResolvedCacheRejectsUnknownExactKey");
    }
    check(catalog_publication().state() == CatalogPublication::State::Empty, "DataOnlyAccessKeepsPublicationEmpty");
    check(k_backend_definition_v1_source_count == 8 && k_backend_definition_v2_source_count == 2 &&
        k_backend_definition_source_count == 10 &&
        k_backend_definition_v1_source_bytes + k_backend_definition_v2_source_bytes == k_backend_definition_source_bytes,
        "SourceInventoryIsExactPartition");
#ifndef LEMON_SOURCE_CACHE_DATA_ONLY
    const auto frozen = freeze_builtin_catalog();
    const auto snapshot_catalog = make_catalog_source_snapshot(frozen, {});
    const auto admission = snapshot_catalog.snapshot ? admit_catalog(snapshot_catalog.snapshot) : CatalogAdmissionResult{};
    check(admission.catalog && admission.catalog->metrics().definitions == 10 &&
        admission.catalog->metrics().source_bytes == k_backend_definition_source_bytes, "BuiltinSourceChargedExactlyOnce");
    const auto built = build_backend_catalog(frozen, {});
    check(bool(built.catalog), "BuiltinCandidateFinalizes");
    if (!built.catalog) {
        for (const auto& error : built.diagnostics.entries) std::printf("  %s %s\n", error.code.c_str(), error.pointer.c_str());
    } else {
        const BackendRegistration* registration = nullptr;
        for (const auto& row : built.catalog->registrations()) if (row.descriptor->recipe == key.id) registration = &row;
        check(registration && registration->descriptor == &source_cache_fixture::descriptor &&
            &registration->configured_entry->published_descriptor() == &source_cache_fixture::descriptor,
            "CanonicalDescriptorSurvivesFinalization");
        if (registration) {
            const auto server = registration->create({"error"});
            check(server->get_descriptor() == &source_cache_fixture::descriptor, "ConfiguredServerUsesCanonicalDescriptor");
            const std::vector<std::function<void(BackendDescriptor&)>> differences{
                [](auto& d) { d.recipe += "-wrong"; },
                [](auto& d) { d.display_name += " wrong"; },
                [](auto& d) { d.binary += "-wrong"; },
                [](auto& d) { d.config_section += "wrong"; },
                [](auto& d) { d.default_device = d.default_device == DEVICE_CPU ? DEVICE_GPU : DEVICE_CPU; },
                [](auto& d) { d.slot_policy = SlotPolicy::Unmetered; },
                [](auto& d) { d.selectable_backend = !d.selectable_backend; },
                [](auto& d) { d.uses_ctx_size = !d.uses_ctx_size; },
                [](auto& d) { d.dynamic_models = !d.dynamic_models; },
                [](auto& d) { d.options[0].name += "wrong"; },
                [](auto& d) { d.options[0].cli_flag += "wrong"; },
                [](auto& d) { d.options[0].default_value = nullptr; },
                [](auto& d) { d.options[0].type_name += "wrong"; },
                [](auto& d) { d.options[0].help += "wrong"; },
                [](auto& d) { d.options[0].group += "wrong"; },
                [](auto& d) { d.support[0].backend += "wrong"; },
                [](auto& d) { d.support[0].tier = BackendTier::Guest; },
                [](auto& d) { d.support[0].format = BackendFormat::Python; },
                [](auto& d) { d.support[0].supported_os.clear(); },
                [](auto& d) { d.support[0].devices.clear(); },
                [](auto& d) { d.support[0].device_summary += "wrong"; },
                [](auto& d) { d.support[0].arch_gates["test"].channels.insert("nightly"); },
                [](auto& d) { d.supported_modes.push_back("embedding"); },
                [](auto& d) { d.required_checkpoints.push_back("extra"); },
                [](auto& d) { d.default_capabilities.push_back("extra"); },
                [](auto& d) { d.web_display_name += "wrong"; },
                [](auto& d) { d.rocm_channels.push_back("wrong"); },
                [](auto& d) { d.exposes_prometheus_metrics = !d.exposes_prometheus_metrics; },
                [](auto& d) { d.rocm_requires_cwsr_fix = !d.rocm_requires_cwsr_fix; },
                [](auto& d) { d.version_policy = VersionPolicy::AtLeast; },
                [](auto& d) { d.self_manages_downloads = !d.self_manages_downloads; },
                [](auto& d) { d.takes_args = !d.takes_args; },
                [](auto& d) { d.arg_variants.push_back("wrong"); },
                [](auto& d) { d.bin_variants.push_back("wrong"); },
                [](auto& d) { d.config_extra["number"] = 1.0; },
                [](auto& d) { d.config_extra["number"] = static_cast<std::int64_t>(1); },
                [](auto& d) { d.streams_model_from_storage = !d.streams_model_from_storage; }
            };
            for (const auto& change : differences) {
                auto wrong = source_cache_fixture::descriptor;
                change(wrong);
                bool rejected = false;
                try { (void)BuiltinEntryBuilder::make(registration->configured_entry->executable_definition, wrong); }
                catch (const std::logic_error&) { rejected = true; }
                check(rejected, "CanonicalDescriptorRequiresCompleteFieldEquality");
            }
        }
        check(catalog_publication().state() == CatalogPublication::State::Empty, "PrivateFinalizationKeepsPublicationEmpty");
        CatalogPublicationGuard publication(catalog_publication());
        publication.commit(built.catalog->data(), built.catalog);
        check(descriptor_for(key.id) == &source_cache_fixture::descriptor, "CanonicalDescriptorSurvivesPublication");
    }
#endif
    std::printf("%d checks, %d failures (%s)\n", checks, failures, order.c_str());
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    try { return run_test(argc, argv); }
    catch (const std::exception& error) {
        std::printf("FAIL BuiltinStartupFailsClosed: %s\n", error.what());
        return 1;
    }
}
