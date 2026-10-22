#include "catalog_admission_fixture.h"
#include "llamacpp_external_catalog_fixture.h"

using namespace admission_fixture;
namespace {
std::shared_ptr<const FrozenBuiltinCatalog> exported(Json base, BuiltinExportContract policy = contract()) {
    const auto bytes = base.dump();
    std::vector<DefinitionSourceView> sources{{{DefinitionNamespace::Builtin, "test-export", 1}, bytes, "PRIVATE_PATH"}};
    const auto input = make_v2_source_snapshot(sources);
    DefinitionResolver resolver(input.snapshot);
    const auto resolved = resolver.resolve({DefinitionNamespace::Builtin, "test-export", 1}).definition;
    policy.expected_source_digest = resolved->source_digest;
    policy.expected_definition_hash = resolved->definition_hash;
    policy.expected_dependency_digest = resolved->dependency_digest;
    repin(policy);
    const auto registry = BuiltinCatalogLoader::test_exports({policy}, input.snapshot, installed_hook_abis(), v2::export_registry_digest({policy}, installed_hook_abis()));
    const auto frozen = BuiltinCatalogLoader::test_freeze(sources, registry);
    return CatalogAdmissionBuilder::test_builtins(frozen.snapshot, sources);
}
}
int main() {
    const auto builtin = freeze_builtin_catalog();
    auto base = root();
    auto middle = child("middle", "root");
    middle["merge"]["metadata"]["display_name"] = "Override";
    auto leaf = child("leaf", "middle");
    const auto active = admit({leaf, middle, base}, builtin);
    check(active.catalog != nullptr, "complete descendant graph admitted");
    if (!active.catalog) return finish();
    const auto identity = active.catalog->admission_id();
    const auto original = active.catalog->v2_definition("leaf");
    base["merge"]["metadata"]["display_name"] = "Changed upstream";
    const auto updated = admit({base, middle, leaf}, builtin);
    check(updated.catalog != nullptr, "unpinned compatible parent replacement admitted");
    if (updated.catalog) {
        const auto changed = updated.catalog->v2_definition("leaf");
        check(changed->definition().definition_hash == original->definition().definition_hash, "overridden transitive data retains effective hash");
        check(changed->definition().dependency_digest != original->definition().dependency_digest, "transitive dependency identity changes behind unchanged middle");
        check(changed.get() != original.get() && &changed->authority() != &original->authority(), "fresh authority for every descendant snapshot");
        check(updated.catalog->metrics().authorized_definitions == 5, "all descendants reauthorized after parent change");
        check(updated.catalog->admission_id() != identity, "changed graph identity invalidates admission");
    }
    leaf["extends"]["expected_digest"] = active.catalog->v2_definition("middle")->definition().definition_hash;
    check(admit({base, middle, leaf}, builtin).catalog != nullptr, "immediate parent pin permits overridden grandparent change");
    const auto private_replacement = validate_candidate_definition(active.catalog, base.dump());
    check(private_replacement.admission.catalog && private_replacement.admission.catalog->metrics().authorized_definitions == 5 &&
        private_replacement.admission.catalog->v2_definition("leaf")->definition().dependency_digest != original->definition().dependency_digest,
        "private base candidate revalidates all descendants");
    middle["extends"]["expected_digest"] = active.catalog->v2_definition("root")->definition().definition_hash;
    const auto pinned = admit({base, middle, leaf}, builtin);
    check(!pinned.catalog && has_code(pinned.diagnostics, "expected_digest_mismatch"), "one stale descendant pin rejects entire transaction");
    const auto pinned_active = admit({root(), middle, leaf}, builtin);
    const auto private_failure = validate_candidate_definition(pinned_active.catalog, base.dump());
    check(!private_failure.admission.catalog && has_code(private_failure.admission.diagnostics, "expected_digest_mismatch"), "private replacement rejects stale descendant pin atomically");
    auto unrelated = base; unrelated["recipe"] = "different";
    const auto mismatched = validate_candidate_definition(active.catalog, unrelated.dump());
    check(!mismatched.admission.catalog && has_code(mismatched.admission.diagnostics, "id_collision"), "candidate replacement requires both id and recipe");
    check(active.catalog->admission_id() == identity && active.catalog->v2_definition("leaf").get() == original.get(), "failed transaction preserves active snapshot");
    middle["extends"].erase("expected_digest");
    check(!admit({middle, leaf}, builtin).catalog, "parent removal rejects complete candidate");
    base["merge"]["launch"].erase("program");
    const auto malformed = admit({base, middle, leaf}, builtin);
    check(!malformed.catalog && !malformed.diagnostics.entries.empty(), "invalid ancestor cannot be repaired by child");
    const auto bytes = Json::parse(read_bytes("export.synthetic.json"));
    auto external = child("external", "unused");
    external["extends"] = {{"builtin", "test-export"}, {"contract", 1}};
    const auto first_builtins = exported(bytes);
    const auto first = admit({external}, first_builtins);
    check(first.catalog != nullptr, "synthetic compiled builtin export composes");
    auto policy = contract();
    ++policy.policy_revision;
    const auto policy_builtins = exported(bytes, policy);
    const auto changed_policy = admit({external}, policy_builtins);
    check(changed_policy.catalog != nullptr, "policy replacement reaudits and reauthorizes");
    if (first.catalog && changed_policy.catalog) {
        const auto old = first.catalog->v2_definition("external");
        const auto changed = changed_policy.catalog->v2_definition("external");
        check(old->definition().definition_hash == changed->definition().definition_hash, "policy change preserves declaration hash");
        check(CatalogAdmissionBuilder::authority_identity(*old) != CatalogAdmissionBuilder::authority_identity(*changed), "policy change invalidates private authority identity");
        check(first.catalog->admission_id() != changed_policy.catalog->admission_id(), "policy change invalidates catalog identity");
    }
    const auto removed = admit({external}, empty_builtins());
    check(!removed.catalog && has_code(removed.diagnostics, "unexported_builtin"), "removed export cannot reuse old authority");
    external["extends"]["contract"] = 2;
    const auto mismatch = admit({external}, first_builtins);
    check(!mismatch.catalog && has_code(mismatch.diagnostics, "contract_mismatch"), "different available contract is explicit mismatch");
    const auto snap = snapshot({root(), child("child", "root")}, builtin);
    const auto once = admit_catalog(snap.snapshot);
    const auto twice = admit_catalog(snap.snapshot);
    check(once.catalog && twice.catalog && once.catalog->metrics().retained_logical_bytes == twice.catalog->metrics().retained_logical_bytes &&
        once.catalog->admission_id() == twice.catalog->admission_id(), "repeat admission has bounded independent accounting");
    lemon::backends::external_fixture::admission_checks(check);
    return finish();
}
