#include "backend_catalog_execution_fixture.h"
#include "backend_catalog_ledger_internal.h"
#include "backend_definition_binding_internal.h"
#include "backend_definition_execution_internal.h"
#include "backend_definition_resources.h"
#include "backend_typed_hook_registry_internal.h"

#include <future>
#include <limits>
#include <utility>

using namespace catalog_execution_fixture;
namespace ac = lemon::backends::accounting;

int main() {
    int shared = 0, equal_but_owned = 0;
    ac::CatalogLedger ledger({});
    {
        ac::StageScope stage(ledger, CatalogBuildStage::Admission);
        ledger.owned(500);
        ledger.receipt(&shared);
        const auto first = ledger.metrics().retained_bytes;
        bool duplicate = false;
        try { ledger.receipt(&shared); } catch (const std::logic_error&) { duplicate = true; }
        check(duplicate, "OwnershipReceiptCannotBeMintedTwice");
        ledger.dependency(&shared);
        ledger.dependency(&shared);
        check(ledger.metrics().retained_bytes == first && ledger.metrics().dependency_references == 2, "SharedDependencyPayloadIsChargedOnce");
        ledger.owned(500);
        ledger.receipt(&equal_but_owned);
        check(ledger.metrics().retained_bytes > first + 500, "EqualOwnedCopiesAreChargedSeparately");
        bool foreign = false;
        try { ledger.dependency(&foreign); } catch (const std::logic_error&) { foreign = true; }
        check(foreign, "ReceiptsRequireExactTransactionObject");
        {
            ac::Temporary temporary(700);
            check(ledger.metrics().peak_bytes == ledger.metrics().retained_bytes + 700, "TemporaryPeakIncludesRetainedOwnership");
        }
        check(ledger.metrics().peak_temporary_bytes == 700, "TemporaryPeakIsNotRefunded");
    }
    ac::CatalogLedger work_ledger({});
    {
        ac::StageScope admission(work_ledger, CatalogBuildStage::Admission);
        work_ledger.work(9, 2);
        { ac::StageScope binding(work_ledger, CatalogBuildStage::Binding); work_ledger.work(7, 3); }
        work_ledger.work(2, 1);
    }
    check(work_ledger.metrics().work_bytes == 18 && work_ledger.metrics().work_visits == 6 &&
        work_ledger.metrics().stages[0].work_bytes == 11 && work_ledger.metrics().stages[2].work_bytes == 7,
        "StageWorkAccumulatesAndNestedScopesRestoreStage");
    ledger.seal(2, 2);
    bool sealed = false;
    try { ledger.owned(1); } catch (const std::logic_error&) { sealed = true; }
    check(sealed, "SealedLedgerCannotChange");
    CatalogExecutionLimits small;
    small.retained_bytes = 499;
    ac::CatalogLedger insufficient(small);
    bool storage_rejected = false;
    try { insufficient.owned(500); } catch (const v2::LimitExceeded&) { storage_rejected = true; }
    check(storage_rejected && insufficient.metrics().retained_bytes == 0, "OwnedStorageLimitRejectsBeforeCounterGrowth");
    ac::CatalogLedger incomplete({});
    bool rejected = false;
    try { incomplete.seal(2, 1); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "BucketCannotSealWithoutEveryFinalizedEntry");

    const std::vector<Json> graph{executable_root(), child("ledger-leaf", "catalog-root")};
    const auto baseline = build(graph);
    check(bool(baseline.catalog), "AccountingFixtureReachesAllStages");
    if (!baseline.catalog) { dump_errors(baseline); return finish(); }
    const auto executable = baseline.catalog->registrations().back().spec->executable_definition;
    auto token = std::shared_ptr<const AuthorizedDefinition>(baseline.catalog, &executable->compiled().authorized());
    ExecutionCompileLimits local;
    local.retained_bytes = token->definition().metrics.retained_logical_bytes + 4096;
    execution::counters = {};
    const auto bounded_compile = compile_execution_definition(token, local);
    check(!bounded_compile.definition && has_code(bounded_compile.diagnostics, "limit_exceeded") && !execution::counters.declaration_copies,
        "NormalizationCopyIsChargedBeforeAllocation");
    std::size_t compiler_transitive = 0, binder_transitive = 0;
    for (const auto& row : baseline.catalog->registrations()) if (row.spec && row.spec->executable_definition) {
        compiler_transitive += row.spec->executable_definition->compiled().metrics().retained_bytes;
        binder_transitive += row.spec->executable_definition->metrics().retained_bytes;
    }
    const auto& metrics = baseline.metrics;
    const auto compile_in_ledger = [&](CatalogExecutionLimits limits) {
        ac::CatalogLedger transaction(limits);
        { ac::StageScope stage(transaction, CatalogBuildStage::Admission); transaction.receipt(&token->definition()); }
        ac::StageScope stage(transaction, CatalogBuildStage::Compilation);
        auto result = compile_execution_definition(token);
        return std::make_pair(std::move(result), transaction.metrics());
    };
    const auto compiled_in_ledger = compile_in_ledger({});
    check(bool(compiled_in_ledger.first.definition), "CompilerReceiptFixtureCompletes");
    CatalogExecutionLimits compiler_receipt_limit;
    compiler_receipt_limit.stages[1].owned_bytes = compiled_in_ledger.second.stages[1].owned_bytes - 1;
    const auto no_compiler_receipt = compile_in_ledger(compiler_receipt_limit);
    check(!no_compiler_receipt.first.definition && has_code(no_compiler_receipt.first.diagnostics, "limit_exceeded"),
        "CompilerReceiptFailureCannotReturnCompletedDefinition");
    const auto admitted = std::shared_ptr<const AdmittedCatalog>(baseline.catalog, &binding::Inspection::admitted(*executable));
    const auto compiled_owner = std::shared_ptr<const CompiledDefinition>(baseline.catalog, &executable->compiled());
    const auto bind_in_ledger = [&](CatalogExecutionLimits limits) {
        ac::CatalogLedger transaction(limits);
        {
            ac::StageScope stage(transaction, CatalogBuildStage::Admission);
            transaction.receipt(admitted.get());
            transaction.receipt(compiled_owner.get());
        }
        ac::StageScope stage(transaction, CatalogBuildStage::Binding);
        auto result = bind_execution_definition(admitted, compiled_owner);
        return std::make_pair(std::move(result), transaction.metrics());
    };
    const auto bound_in_ledger = bind_in_ledger({});
    check(bool(bound_in_ledger.first.definition), "BinderReceiptFixtureCompletes");
    CatalogExecutionLimits binder_receipt_limit;
    binder_receipt_limit.stages[2].owned_bytes = bound_in_ledger.second.stages[2].owned_bytes - 1;
    const auto no_binder_receipt = bind_in_ledger(binder_receipt_limit);
    check(!no_binder_receipt.first.definition && has_code(no_binder_receipt.first.diagnostics, "limit_exceeded"),
        "BinderReceiptFailureCannotReturnCompletedDefinition");
    check(metrics.stages[1].peak_temporary_bytes >= compiled_in_ledger.second.stages[1].peak_temporary_bytes + k_backend_definition_source_bytes,
        "CompilationReservesInventoryAlongsideProgramTemporaryStorage");
    check(metrics.stages[1].owned_bytes < compiler_transitive, "CompilerDoesNotRebillResolvedDependencies");
    check(metrics.stages[2].owned_bytes < binder_transitive, "BinderDoesNotRebillAdmittedAndCompiledDependencies");
    std::printf("ledger retained=%zu temporary=%zu peak=%zu work_bytes=%zu work_visits=%zu references=%zu\n",
        metrics.retained_bytes, metrics.peak_temporary_bytes, metrics.peak_bytes, metrics.work_bytes, metrics.work_visits, metrics.dependency_references);
    std::size_t owned = 0, bytes = 0, visits = 0;
    for (const auto& stage : metrics.stages) { owned += stage.owned_bytes; bytes += stage.work_bytes; visits += stage.work_visits; }
    check(owned == metrics.retained_bytes && bytes == metrics.work_bytes && visits == metrics.work_visits && metrics.dependency_references == 10,
        "StageTotalsDescribeUniqueOwnershipAndCumulativeWork");
    check(metrics.peak_temporary_bytes > 0 && metrics.peak_bytes >= metrics.retained_bytes && metrics.stages[3].owned_bytes > 0, "AllFinalizationAndTemporaryStorageIsCharged");
    const auto empty = build({});
    check(metrics.stages[3].owned_bytes > empty.metrics.stages[3].owned_bytes + 2 * sizeof(ConfiguredBackendEntry), "EveryEntryAndProjectionReservesOwnedStorage");
    check(metrics.stages[3].peak_temporary_bytes == 2 * metrics.stages[3].owned_bytes + 8192,
        "FinalSchemaAndIdentityTemporaryReservationIsCharged");
    CatalogExecutionLimits exact;
    exact.retained_bytes = metrics.peak_bytes;
    exact.temporary_bytes = metrics.peak_temporary_bytes;
    exact.work_bytes = metrics.work_bytes;
    exact.work_visits = metrics.work_visits;
    check(bool(build(graph, exact).catalog), "ExactAggregateLimitsPass");
    for (const auto field : {0, 1, 2, 3}) {
        auto limit = exact;
        if (field == 0) --limit.retained_bytes;
        if (field == 1) --limit.temporary_bytes;
        if (field == 2) --limit.work_bytes;
        if (field == 3) --limit.work_visits;
        const auto failed = build(graph, limit);
        check(!failed.catalog && has_code(failed.diagnostics, "limit_exceeded"), "AggregateOneUnderRejects:" + std::to_string(field));
    }
    for (std::size_t stage = 0; stage < 4; ++stage) {
        for (const auto field : {0, 1, 2, 3}) {
            auto limit = exact;
            const auto& measured = metrics.stages[stage];
            if (field == 0) limit.stages[stage].owned_bytes = measured.owned_bytes - 1;
            if (field == 1) limit.stages[stage].work_bytes = measured.work_bytes ? measured.work_bytes - 1 : 0;
            if (field == 2) limit.stages[stage].work_visits = measured.work_visits - 1;
            if (field == 3) limit.stages[stage].peak_temporary_bytes = measured.peak_temporary_bytes - 1;
            if (field == 1 && !measured.work_bytes) continue;
            check(!build(graph, limit).catalog, "IndependentStageBound:" + std::to_string(stage) + ":" + std::to_string(field));
        }
    }
    auto zero = CatalogExecutionLimits{};
    zero.retained_bytes = 0;
    const auto frozen_for_early = freeze_builtin_catalog();
    v2::admission_counters = {};
    const auto early = build(graph, zero, frozen_for_early);
    check(!early.catalog && has_code(early.diagnostics, "limit_exceeded") && !v2::admission_counters.dom && !v2::admission_counters.clones,
        "AggregateStorageCheckedBeforeAdmissionAllocation");
    zero = {};
    zero.stages[1].owned_bytes = 0;
    const auto no_compile = build(graph, zero);
    check(!no_compile.catalog && no_compile.stages.catalog_admitted && !no_compile.stages.compiled, "CompilationStorageCheckedBeforeProgramGrowth");
    zero = {};
    zero.diagnostics = 0;
    zero.retained_bytes = 0;
    check(!build(graph, zero).catalog, "DiagnosticExhaustionNeverCreatesSuccess");
    const auto frozen = freeze_builtin_catalog();
    auto reduced = std::async(std::launch::async, [&] { CatalogExecutionLimits limit; limit.work_bytes = 0; return build(graph, limit, frozen); });
    auto independent = std::async(std::launch::async, [&] { return build(graph, {}, frozen); });
    const auto rejected_parallel = reduced.get();
    const auto valid_parallel = independent.get();
    check(!rejected_parallel.catalog && valid_parallel.catalog && valid_parallel.metrics.retained_bytes == baseline.metrics.retained_bytes &&
        valid_parallel.metrics.work_bytes == baseline.metrics.work_bytes, "ConcurrentTransactionsHaveIndependentLedgers");
    auto no_diagnostics = CatalogExecutionLimits{};
    no_diagnostics.diagnostics = 0;
    auto invalid = executable_root(); invalid["merge"]["kind"] = "unavailable";
    const auto diagnostic_failure = build({invalid}, no_diagnostics);
    check(!diagnostic_failure.catalog && diagnostic_failure.diagnostics.entries.empty() && diagnostic_failure.diagnostics.truncated,
        "DiagnosticBoundAppliesAcrossAdmissionAndCompilation");
    invalid = executable_root(); invalid["merge"]["operations"][0]["transport"]["kind"] = "http_bytes";
    const auto compiler_diagnostic = build({invalid}, no_diagnostics);
    check(!compiler_diagnostic.catalog && compiler_diagnostic.diagnostics.entries.empty() && compiler_diagnostic.diagnostics.truncated,
        "DiagnosticBoundIncludesCompilerFailures");
    CatalogExecutionLimits overflow;
    overflow.retained_bytes = std::numeric_limits<std::size_t>::max();
    ac::CatalogLedger clamped(overflow);
    check(clamped.limits().retained_bytes == CatalogExecutionLimits{}.retained_bytes, "PublicBoundsOnlyReduceHardLimits");
    return finish();
}

void lemon::backends::typed::observe_entry(const char*) { throw std::logic_error("CatalogValidationInvokesNothing"); }
