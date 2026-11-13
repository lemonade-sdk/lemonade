#include "backend_definition_binding_fixture.h"

#include <limits>

using namespace binding_fixture;

int main() {
    try {
        const auto value = fixture();
        const auto result = bind(value);
        check(bool(result.definition), "HookBearingBoundedBaseline");
        if (!result.definition) return finish();
        const auto metrics = result.definition->metrics();
        const auto failed = [&](const ExecutionBindLimits& limits, const std::string& label) {
            const auto result = bind(value, limits);
            check(!result.definition && (has_code(result.diagnostics, "limit_exceeded") || result.diagnostics.truncated), label);
            zero(label);
        };
        ExecutionBindLimits limits;
        for (auto field : {&ExecutionBindLimits::slots, &ExecutionBindLimits::retained_bytes, &ExecutionBindLimits::work_visits}) {
            const auto amount = field == &ExecutionBindLimits::slots ? metrics.slots :
                field == &ExecutionBindLimits::retained_bytes ? metrics.retained_bytes : metrics.work_visits;
            limits = {}; limits.*field = amount;
            check(bool(bind(value, limits).definition), "ExactBindingBudgetAccepted");
            limits.*field = amount - 1;
            failed(limits, "OneUnderBindingBudgetRejected");
        }
        limits = {}; limits.slots = 0;
        bd::counters = {}; failed(limits, "SlotsPrecharged");
        check(bd::counters.slot_copies == 0 && bd::counters.target_checks == 0, "SlotStorageChargedBeforeCopyOrAudit");
        limits = {}; limits.retained_bytes = value.admitted->metrics().retained_logical_bytes + value.compiled->metrics().retained_bytes - 1;
        bd::counters = {}; failed(limits, "RetainedContextPrecharged");
        check(bd::counters.membership_scans == 0 && bd::counters.slot_copies == 0, "RetainedContextChargedBeforeScanOrCopy");
        limits = {}; limits.work_visits = value.admitted->metrics().authorized_definitions;
        bd::counters = {}; failed(limits, "MembershipScanPrecharged");
        check(bd::counters.membership_scans == 0, "MembershipWorkChargedBeforeScan");
        limits = {}; limits.work_visits = value.admitted->metrics().authorized_definitions + 1;
        bd::counters = {}; failed(limits, "DependencyTraversalPrecharged");
        check(bd::counters.target_checks == 0 && bd::counters.slot_copies == 0, "DependencyWorkChargedBeforeTargetCopy");
        auto many = document(true);
        many["merge"]["launch"]["argv"][0]["when"] = {{"all", std::vector<bool>(2500, true)}};
        const auto many_admitted = admit({}, builtins(true, &many));
        check(bool(many_admitted.catalog), "ExpressionWorkFixtureAdmitted");
        if (!many_admitted.catalog) return finish();
        const auto many_token = many_admitted.catalog->builtin_v2_definition({DefinitionNamespace::Builtin, "binding-hrx", 1});
        const auto many_compiled = compile_execution_definition(many_token);
        check(bool(many_compiled.definition), "ExpressionWorkFixtureCompiles");
        if (!many_compiled.definition) { diagnostics(many_compiled.diagnostics); return finish(); }
        limits = {}; limits.work_visits = many_compiled.definition->metrics().expression_nodes + many_admitted.catalog->metrics().authorized_definitions + 1;
        bd::counters = {};
        const auto many_result = bind_execution_definition(many_admitted.catalog, many_compiled.definition, limits);
        check(!many_result.definition && bd::counters.slot_copies == 0, "ExpressionTraversalChargedBeforeCopy");
        limits = {}; limits.retained_bytes = metrics.retained_bytes - 1;
        bd::counters = {}; failed(limits, "IdentityConstructionPrecharged");
        check(bd::counters.identities == 0, "IdentityChargedBeforeConstruction");
        limits = {}; limits.slots = 0; limits.diagnostics = 0;
        const auto silent = bind(value, limits);
        check(!silent.definition && silent.diagnostics.truncated && silent.diagnostics.entries.empty(), "ZeroDiagnosticsStillReturnsNull");
        limits.diagnostics = 1;
        const auto capped = bind(value, limits);
        check(!capped.definition && capped.diagnostics.entries.size() == 1, "CappedDiagnosticsStillReturnsNull");
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto clamped = bd::bounded({maximum, maximum, maximum, maximum});
        const ExecutionBindLimits defaults;
        check(clamped.slots == defaults.slots && clamped.retained_bytes == defaults.retained_bytes &&
            clamped.work_visits == defaults.work_visits && clamped.diagnostics == defaults.diagnostics, "LimitsOnlyReduceDefaults");
        const auto widened = bind(value, {maximum, maximum, maximum, maximum});
        check(widened.definition && widened.definition->execution_id() == result.definition->execution_id() &&
            widened.definition->metrics().retained_bytes == metrics.retained_bytes, "RaisedLimitsCannotAlterAccountingOrIdentity");
        for (unsigned which = 0; which < 5; ++which) {
            bool caught = false;
            try {
                bd::Budget budget{bd::bounded({maximum, maximum, maximum, maximum}), {}};
                if (which == 0) { budget.metrics.retained_bytes = maximum; budget.keep(1); }
                if (which == 1) { budget.metrics.work_visits = maximum; budget.work(1); }
                if (which == 2) { budget.metrics.slots = maximum; budget.slots(1); }
                if (which == 3) budget.keep(v2::checked_product(maximum, sizeof(bd::BoundHook)));
                if (which == 4) budget.work(v2::checked_sum(maximum, 1));
            } catch (const v2::LimitExceeded&) { caught = true; }
            check(caught, "CheckedBindingArithmeticNeverWraps");
        }
        for (const auto& limit : std::vector<ExecutionBindLimits>{{0,0,0,0},{0,0,0,1},{4096,1,1048576,0},{4096,128*1024*1024,0,0}}) {
            const auto rejected = bind(value, limit);
            check(!rejected.definition, "EveryFailureReturnsNullNeverPartialObject");
        }
        zero("all binding limit failures");
    } catch (const std::exception& error) { check(false, error.what()); }
    return finish();
}
