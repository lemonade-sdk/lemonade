#include "backend_definition_execution_fixture.h"

#include <limits>

using namespace execution_fixture;

int main() {
    const auto token = authorize(expression_document(true));
    const auto authored = token->definition().declaration;
    const auto success = compile_execution_definition(token);
    check(bool(success.definition), "BaselineCompiles");
    if (!success.definition) return 1;
    const auto metrics = success.definition->metrics();
    const auto failure = [&](const ExecutionCompileLimits& limits, const std::string& name, bool before_copy = false) {
        ex::counters = {};
        const auto result = compile_execution_definition(token, limits);
        check(code(result, "limit_exceeded") && !result.definition, name);
        if (before_copy) check(ex::counters.declaration_copies == 0, name + ":BeforeCopy");
        check(token->definition().declaration == authored, name + ":RetainedSourceIsUnchangedAfterEveryFailure");
    };
    ExecutionCompileLimits limits;
    limits.normalized_bytes = 0; failure(limits, "CopyRejectedBeforeAllocation", true);
    limits = {}; limits.normalized_nodes = 0; failure(limits, "NodesRejectedBeforeAllocation", true);
    limits = {}; limits.normalized_bytes = token->definition().metrics.effective_bytes; failure(limits, "NormalizationExpansionIsCharged", true);
    limits = {}; limits.retained_bytes = token->definition().metrics.retained_logical_bytes; failure(limits, "RetainedBudgetBeforeCopy", true);
    ex::Budget raw_budget{{}, {}};
    raw_budget.copy(authored);
    limits = {}; limits.retained_bytes = token->definition().metrics.retained_logical_bytes + raw_budget.metrics.retained_bytes - 1;
    failure(limits, "ExecutionCopyChargedBeforeAllocation", true);
    limits = {}; limits.work_bytes = 0; failure(limits, "WorkBytesBeforeCopy", true);
    limits = {}; limits.work_visits = 0; failure(limits, "WorkVisitsBeforeCopy", true);
    limits = {}; limits.expression_nodes = 0; failure(limits, "FailurePublishesNoPartialProgram");
    for (auto field : {&ExecutionCompileLimits::normalized_bytes, &ExecutionCompileLimits::normalized_nodes,
        &ExecutionCompileLimits::work_bytes, &ExecutionCompileLimits::work_visits, &ExecutionCompileLimits::retained_bytes}) {
        const auto metric = field == &ExecutionCompileLimits::normalized_bytes ? metrics.normalized_bytes :
            field == &ExecutionCompileLimits::normalized_nodes ? metrics.normalized_nodes :
            field == &ExecutionCompileLimits::work_bytes ? metrics.work_bytes :
            field == &ExecutionCompileLimits::work_visits ? metrics.work_visits : metrics.retained_bytes;
        limits = {}; limits.*field = metric;
        check(bool(compile_execution_definition(token, limits).definition), "ExactBudgetBoundaryAccepted");
        limits.*field = metric - 1;
        failure(limits, "OneBelowBudgetRejected");
    }
    auto value = expression_document(true);
    value["merge"]["launch"]["argv"] = Json::array({{{"id", "one"}, {"args", Json::array({{{"ref", "/model/id"}}, {{"ref", "/model/id"}}})}}});
    const auto repeated = authorize(value);
    const auto repeated_result = compile_execution_definition(repeated);
    check(repeated_result.definition->metrics().expression_nodes == metrics.expression_nodes + 3, "RepeatedReferencesStillChargeWork");
    limits = {}; limits.expression_nodes = repeated_result.definition->metrics().expression_nodes - 1;
    check(code(compile_execution_definition(repeated, limits), "limit_exceeded"), "ExpressionNodesAreGlobalPerDefinition");
    Json nested = true;
    for (int i = 0; i < 4; ++i) nested = Json{{"not", nested}};
    const auto depth_token = authorize(expression_document(nested));
    limits = {}; limits.expression_depth = 4;
    check(bool(compile_execution_definition(depth_token, limits).definition), "DepthBoundaryIsExact");
    limits.expression_depth = 3;
    check(code(compile_execution_definition(depth_token, limits), "limit_exceeded"), "DepthAboveBoundaryRejected");
    ex::Budget budget{{}, {}};
    budget.metrics.retained_bytes = std::numeric_limits<std::size_t>::max();
    bool overflow = false;
    try { budget.keep(1); } catch (const v2::LimitExceeded&) { overflow = true; }
    check(overflow, "OverflowDoesNotWrap");
    limits = {}; limits.expression_nodes = 0; limits.diagnostics = 0;
    const auto silent = compile_execution_definition(token, limits);
    check(!silent.definition && silent.diagnostics.entries.empty() && silent.diagnostics.truncated, "DiagnosticsAreBounded");
    limits = {}; limits.normalized_bytes = 0; limits.diagnostics = std::numeric_limits<std::size_t>::max();
    check(compile_execution_definition(token, limits).diagnostics.entries.size() == 1, "LimitsCannotWidenHardCeilings");
    auto diagnostic = compile(expression_document({{"ref", "/PRIVATE_ENV/PRIVATE_ARGV/PRIVATE_PATH"}}));
    check(code(diagnostic, "invalid_reference") && diagnostic.diagnostics.entries[0].message.find("PRIVATE") == std::string::npos && diagnostic.diagnostics.entries[0].pointer.find("PRIVATE") == std::string::npos, "DiagnosticsRedactValues");
    Json many = Json::array();
    for (int i = 0; i < 4097; ++i) many.push_back(true);
    const auto hard_token = authorize(expression_document({{"all", many}}));
    limits = {}; limits.expression_nodes = std::numeric_limits<std::size_t>::max();
    check(code(compile_execution_definition(hard_token, limits), "limit_exceeded"), "CallerCannotWidenExpressionCeiling");
    return finish();
}
