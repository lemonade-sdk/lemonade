#include "backend_catalog_admission_internal.h"
#include "backend_definition_binding_internal.h"

#include <algorithm>
#include <set>
#include <type_traits>

namespace lemon::backends::binding {

#ifdef LEMONADE_BINDING_TEST
thread_local Counters counters;
thread_local const std::vector<typed::Registration>* test_targets = nullptr;
#define LEMON_BIND_COUNT(field) (++::lemon::backends::binding::counters.field)
#else
#define LEMON_BIND_COUNT(field) ((void)0)
#endif

ExecutionBindLimits bounded(ExecutionBindLimits value) {
    const ExecutionBindLimits hard;
    value.slots = std::min(value.slots, hard.slots);
    value.retained_bytes = std::min(value.retained_bytes, hard.retained_bytes);
    value.work_visits = std::min(value.work_visits, hard.work_visits);
    value.diagnostics = std::min(value.diagnostics, hard.diagnostics);
    return value;
}
void Budget::keep(std::size_t bytes) { v2::charge(metrics.retained_bytes, bytes, limits.retained_bytes); }
void Budget::work(std::size_t visits) { v2::charge(metrics.work_visits, visits, limits.work_visits); }
void Budget::slots(std::size_t count) { v2::charge(metrics.slots, count, limits.slots); }

namespace {
void precharge(const execution::Program& program, const AuthorizedDefinition& token, Budget& budget) {
    const auto count = program.hooks.size();
    budget.slots(count);
    budget.keep(v2::checked_product(count, sizeof(BoundHook) + 4096));
    budget.work(v2::checked_product(v2::checked_sum(count, 1),
        v2::checked_sum(v2::checked_product(AuthorityInspection::policy_count(token), 4), 64)));
    budget.work(program.expressions.size());
    for (const auto& [pointer, expression] : program.expressions) budget.work(pointer.size());
    for (const auto& hook : program.hooks) {
        const auto bytes = v2::checked_sum(hook.pointer.size(), v2::checked_sum(hook.slot.owner_id.size(), hook.slot.hook_id.size()));
        budget.keep(v2::checked_product(bytes, 16));
        budget.work(v2::checked_sum(bytes, v2::checked_sum(hook.inputs.size(), hook.dependencies.size())));
    }
    for (std::size_t i = 0; i < AuthorityInspection::grant_count(token); ++i) {
        const auto& policy = AuthorityInspection::policy(token, i);
        budget.work(v2::checked_product(v2::checked_sum(policy.required_slots.size(), policy.required_phases.size()), count));
        budget.keep(v2::checked_product(policy.required_slots.size(), sizeof(std::size_t)));
    }
}

void verify_dependencies(const AuthorizedDefinition& token, const execution::Program& program) {
    for (const auto& hook : program.hooks) {
        execution::require(hook.inputs == execution::fact_inputs(program, hook) &&
            hook.dependencies == execution::slot_dependencies(token, program, hook), "sealed_dependency_mismatch", hook.pointer);
    }
    const auto inspect_expression = [&](const auto& self, const execution::Expression& expression) -> void {
        if (expression.producer || expression.fact_field) {
            const auto expected = execution::model_fact(expression.reference, program);
            execution::require(expression.producer == expected.first && expression.fact_field == expected.second,
                "sealed_dependency_mismatch", "");
        } else execution::require(expression.reference.rfind("/launch/hook/", 0) != 0, "sealed_dependency_mismatch", "");
        for (const auto& child : expression.children) self(self, child);
    };
    for (const auto& [pointer, expression] : program.expressions) inspect_expression(inspect_expression, expression);
    std::vector<unsigned> state(program.hooks.size());
    const auto visit = [&](const auto& self, std::size_t index) -> void {
        execution::require(index < program.hooks.size() && state[index] != 1, "phase_dependency", "");
        if (state[index] == 2) return;
        state[index] = 1;
        const auto& hook = program.hooks[index];
        for (const auto& input : hook.inputs) if (input.producer) self(self, *input.producer);
        for (const auto dependency : hook.dependencies) self(self, dependency);
        state[index] = 2;
    };
    for (std::size_t i = 0; i < program.hooks.size(); ++i) visit(visit, i);
}

}

std::string execution_identity(const AdmittedCatalog& admitted, const CompiledDefinition& compiled, Budget& budget, unsigned revision) {
    const auto bytes = v2::checked_sum(admitted.admission_id().size(), compiled.compilation_id().size());
    budget.keep(v2::checked_sum(v2::checked_product(bytes, 8), 4096));
    budget.work(v2::checked_sum(bytes, 128));
    LEMON_BIND_COUNT(identities);
    const execution::Json value = {{"admission", admitted.admission_id()}, {"compilation", compiled.compilation_id()},
        {"binder", revision}, {"targets", 1}};
    DefinitionLimits limits;
    limits.work_nodes = 128;
    limits.work_bytes = 8192;
    limits.retained_logical_bytes = budget.limits.retained_bytes - budget.metrics.retained_bytes;
    std::size_t retained = 0;
    v2::Budget hashing{limits, {}, retained};
    const auto result = v2::digest("lemonade.backend-execution.v1\n", value, hashing);

    return result;
}
}

namespace lemon::backends {

struct ExecutableDefinition::Impl {
    Impl(std::shared_ptr<const AdmittedCatalog> catalog, std::shared_ptr<const CompiledDefinition> program,
        std::vector<binding::BoundHook>&& staging, std::string identity, ExecutionBindMetrics accounting)
        : admitted(std::move(catalog)), compiled(std::move(program)), hooks(std::move(staging)),
          execution_id(std::move(identity)), metrics(accounting) {}
    const std::shared_ptr<const AdmittedCatalog> admitted;
    const std::shared_ptr<const CompiledDefinition> compiled;
    const std::vector<binding::BoundHook> hooks;
    const std::string execution_id;
    const ExecutionBindMetrics metrics;
};

class DefinitionExecutionBinder {
public:
    static ExecutionBindResult bind(std::shared_ptr<const AdmittedCatalog> admitted,
        std::shared_ptr<const CompiledDefinition> compiled, ExecutionBindLimits limits) {
        ExecutionBindResult result;
        binding::Budget budget{binding::bounded(limits), {}};
        const auto fail = [&](const std::string& code, const std::string& pointer) {
            result.definition.reset();
            if (!budget.limits.diagnostics) { result.diagnostics.truncated = true; return; }
            result.diagnostics.entries.push_back({code, "", pointer, "Execution binding failed", {}, {}, {}});
        };
        try {
            execution::require(admitted && compiled, "invalid_binding_input", "");
            budget.keep(v2::checked_sum(admitted->metrics().retained_logical_bytes, compiled->metrics().retained_bytes));
            budget.keep(sizeof(ExecutableDefinition) + sizeof(ExecutableDefinition::Impl));
            budget.work(v2::checked_sum(admitted->metrics().authorized_definitions, 1));
            LEMON_BIND_COUNT(membership_scans);
            execution::require(CatalogAdmissionBuilder::contains_token(*admitted, compiled->authorized()), "token_not_admitted", "");
            const auto& token = compiled->authorized();
            const auto& program = execution::Inspection::program(*compiled);
            budget.work(compiled->metrics().expression_nodes);
            binding::precharge(program, token, budget);
            execution::require(program.hooks.size() == AuthorityInspection::grant_count(token), "sealed_slot_mismatch", "");
            LEMON_BIND_COUNT(target_checks);
            const auto* targets = &typed::installed_targets();
#ifdef LEMONADE_BINDING_TEST
            if (binding::test_targets) targets = binding::test_targets;
#endif
            typed::audit_targets(*targets);
            std::set<std::size_t> ordinals;
            std::vector<binding::BoundHook> staging;
            staging.reserve(program.hooks.size());
            for (std::size_t index = 0; index < program.hooks.size(); ++index) {
                const auto& hook = program.hooks[index];
                execution::verify_symbol(token, hook);
                execution::require(ordinals.insert(hook.grant_ordinal).second, "sealed_slot_mismatch", hook.pointer);
                const auto found = std::find_if(targets->begin(), targets->end(), [&](const auto& entry) { return entry.contract == hook.contract; });
                execution::require(found != targets->end() && found->abi_revision == hook.abi_revision &&
                    found->implementation_revision == hook.implementation_revision && found->effects == hook.effects &&
                    found->provides == hook.provides, "sealed_target_mismatch", hook.pointer);
                LEMON_BIND_COUNT(slot_copies);
                staging.push_back({index, found->target});
            }
            binding::verify_dependencies(token, program);
            auto identity = binding::execution_identity(*admitted, *compiled, budget);
            auto impl = std::make_shared<const ExecutableDefinition::Impl>(std::move(admitted), std::move(compiled),
                std::move(staging), std::move(identity), budget.metrics);
            static_assert(std::is_const_v<typename decltype(impl)::element_type>, "ExecutableDefinitionImplIsActuallyConst");
            result.definition = std::shared_ptr<const ExecutableDefinition>(new ExecutableDefinition(std::move(impl)));
        } catch (const execution::Failure& error) { fail(error.code, error.pointer); }
        catch (const v2::LimitExceeded&) { fail("limit_exceeded", ""); }
        catch (const std::exception&) { fail("sealed_target_mismatch", ""); }
        return result;
    }
};

ExecutableDefinition::ExecutableDefinition(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
const CompiledDefinition& ExecutableDefinition::compiled() const noexcept { return *impl_->compiled; }
const std::string& ExecutableDefinition::execution_id() const noexcept { return impl_->execution_id; }
const ExecutionBindMetrics& ExecutableDefinition::metrics() const noexcept { return impl_->metrics; }
const std::vector<binding::BoundHook>& binding::Inspection::hooks(const ExecutableDefinition& definition) { return definition.impl_->hooks; }
const AdmittedCatalog& binding::Inspection::admitted(const ExecutableDefinition& definition) { return *definition.impl_->admitted; }
ExecutionBindResult bind_execution_definition(std::shared_ptr<const AdmittedCatalog> admitted,
    std::shared_ptr<const CompiledDefinition> compiled, const ExecutionBindLimits& limits) {
    return DefinitionExecutionBinder::bind(std::move(admitted), std::move(compiled), limits);
}

}
