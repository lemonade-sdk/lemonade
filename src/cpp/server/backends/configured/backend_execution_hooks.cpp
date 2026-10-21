#include "backend_definition_execution_internal.h"
#include "backend_definition_exports_internal.h"

#include <algorithm>
#include <set>

namespace lemon::backends::execution {

bool same_slot(const LogicalHookSlot& left, const LogicalHookSlot& right) {
    return left.owner_kind == right.owner_kind && left.owner_id == right.owner_id &&
        left.stage == right.stage && left.hook_id == right.hook_id;
}

HookShape hook_shape(typed::Contract contract) {
    using C = typed::Contract;
    using E = typed::Effect;
    using F = typed::Fact;
    switch (contract) {
        case C::Backend: return {contract, "llamacpp.backend.v1", "backend", 1, 1,
            {E::SupportQuery, E::DeviceQuery}, {F::BackendOptions}, {F::SelectedBackend}};
        case C::InstallContext: return {contract, "llamacpp.install_context.v1", "install_context", 1, 1,
            {E::InstallFactRead}, {F::SelectedBackend, F::Platform, F::VersionInputs}, {F::InstallFacts}};
        case C::RuntimeOptions: return {contract, "llamacpp.runtime_options.v1", "runtime_options", 1, 1,
            {}, {F::ModelLabels, F::DraftCheckpoint}, {F::RuntimeFacts}};
        case C::Prepare: return {contract, "llamacpp.prepare.v1", "launch", 1, 1,
            {}, {F::ModelArtifacts, F::ModelLabels, F::DraftCheckpoint, F::ContextSize, F::SelectedBackend}, {F::ModelFacts}};
        case C::Environment: return {contract, "llamacpp.environment.v1", "environment", 1, 1,
            {E::ParentEnvironmentRead, E::RuntimeFactRead, E::StagingPlan},
            {F::Platform, F::SelectedBackend, F::ResolvedProgram}, {F::EnvironmentPlan}};
        case C::System: return {contract, "llamacpp.system.v1", "system", 1, 1,
            {E::SystemProbe}, {F::Platform, F::SelectedBackend, F::VersionInputs}, {F::SystemFacts}};
        case C::Artifacts: return {contract, "llamacpp.artifacts.v1", "artifacts", 1, 1,
            {E::ArtifactRead}, {F::ModelArtifacts, F::ModelLabels}, {F::ArtifactMetadata}};
        case C::MaxTokensAlias: return {contract, "llamacpp.max_tokens_alias", "request", 1, 1, {}, {F::Request}, {F::Request}};
        case C::ToolSchemaLimits: return {contract, "llamacpp.tool_schema_limits", "request", 1, 1, {}, {F::Request}, {F::Request}};
        case C::ClientModel: return {contract, "llamacpp.client_model", "response", 1, 1, {}, {F::Request, F::Response}, {F::Response}};
    }
    throw Failure{"unsupported_execution_feature", ""};
}

void verify_symbol(const AuthorizedDefinition& token, const SymbolicHook& hook) {
    require(hook.grant_ordinal < AuthorityInspection::grant_count(token), "sealed_slot_mismatch", hook.pointer);
    const auto& grant = AuthorityInspection::grant(token, hook.grant_ordinal);
    const auto& policy = AuthorityInspection::policy(token, hook.grant_ordinal);
    const auto shape = hook_shape(hook.contract);
    const auto order = std::find(policy.ordered_hooks.begin(), policy.ordered_hooks.end(), grant.slot.hook_id);
    require(same_slot(hook.slot, grant.slot) && grant.slot.hook_id == shape.id && grant.slot.stage == shape.stage &&
        hook.parameter_digest == grant.parameter_digest && hook.owner_digest == policy.owner_semantics_digest &&
        hook.policy_digest == grant.mutation_policy_digest && order != policy.ordered_hooks.end() &&
        hook.order == static_cast<std::size_t>(order - policy.ordered_hooks.begin()) &&
        hook.abi_revision == grant.abi_revision && hook.abi_revision == shape.abi_revision &&
        hook.implementation_revision == shape.implementation_revision && hook.effects == shape.effects &&
        hook.provides == shape.provides, "sealed_slot_mismatch", hook.pointer);
    require(hook.inputs.size() == shape.requires.size(), "sealed_dependency_mismatch", hook.pointer);
    for (std::size_t i = 0; i < hook.inputs.size(); ++i)
        require(hook.inputs[i].fact == shape.requires[i] && hook.inputs[i].index == static_cast<std::size_t>(shape.requires[i]),
            "sealed_dependency_mismatch", hook.pointer);
}

std::vector<FactInput> fact_inputs(const Program& program, const SymbolicHook& hook) {
    std::vector<FactInput> result;
    for (const auto fact : hook_shape(hook.contract).requires) {
        std::optional<std::size_t> producer;
        if (fact == typed::Fact::SelectedBackend || fact > typed::Fact::Response) {
            for (std::size_t i = 0; i < program.hooks.size(); ++i) {
                const auto& candidate = program.hooks[i];
                if (std::find(candidate.provides.begin(), candidate.provides.end(), fact) != candidate.provides.end()) {
                    require(!producer && candidate.slot.owner_kind == SlotOwnerKind::Root &&
                        candidate.contract == typed::Contract::Backend && fact == typed::Fact::SelectedBackend,
                        "phase_dependency", hook.pointer);
                    producer = i;
                }
            }
            require(producer.has_value(), "phase_dependency", hook.pointer);
        }
        result.push_back({fact, static_cast<std::size_t>(fact), producer});
    }
    return result;
}

std::vector<std::size_t> slot_dependencies(const AuthorizedDefinition& token, const Program& program, const SymbolicHook& hook) {
    std::vector<std::size_t> result;
    const auto& policy = AuthorityInspection::policy(token, hook.grant_ordinal);
    for (const auto& required : policy.required_slots) {
        const auto found = std::find_if(program.hooks.begin(), program.hooks.end(), [&](const auto& item) { return same_slot(item.slot, required); });
        require(found != program.hooks.end() && !same_slot(found->slot, hook.slot), "sealed_dependency_mismatch", hook.pointer);
        result.push_back(static_cast<std::size_t>(found - program.hooks.begin()));
    }
    for (const auto& stage : policy.required_phases) {
        require(std::any_of(program.hooks.begin(), program.hooks.end(), [&](const auto& item) {
            return item.slot.owner_kind == SlotOwnerKind::Root && item.slot.stage == stage;
        }), "sealed_dependency_mismatch", hook.pointer);
    }
    return result;
}

void compile_hooks(const AuthorizedDefinition& token, Program& program, Budget& budget) {
    const auto count = AuthorityInspection::grant_count(token);
    budget.keep(v2::checked_product(count, sizeof(SymbolicHook) + 4096));
    budget.work(0, v2::checked_product(v2::checked_sum(count, 1), v2::checked_sum(AuthorityInspection::policy_count(token), 64)));
    DefinitionLimits limits;
    limits.work_bytes = budget.limits.work_bytes - budget.metrics.work_bytes;
    limits.work_nodes = budget.limits.work_visits - budget.metrics.work_visits;
    limits.retained_logical_bytes = budget.limits.retained_bytes - budget.metrics.retained_bytes;
    std::size_t retained = 0;
    v2::Budget traversal{limits, {}, retained};
    const auto selections = v2::hook_selections(token.definition().declaration, traversal);
    budget.work(traversal.metrics.work_bytes, traversal.metrics.work_nodes);
    budget.keep(retained);
    require(selections.size() == count, "unsupported_execution_feature", "");
    program.hooks.reserve(count);
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
        const auto& grant = AuthorityInspection::grant(token, ordinal);
        const auto& policy = AuthorityInspection::policy(token, ordinal);
        const auto selected = std::find_if(selections.begin(), selections.end(), [&](const auto& item) { return same_slot(item.slot, grant.slot); });
        require(selected != selections.end(), "sealed_slot_mismatch", "");
        std::optional<HookShape> shape;
        for (unsigned i = 0; i < 10; ++i) {
            auto candidate = hook_shape(static_cast<typed::Contract>(i));
            if (grant.slot.hook_id == candidate.id && grant.slot.stage == candidate.stage) shape = std::move(candidate);
        }
        require(shape.has_value(), "unsupported_execution_feature", selected->pointer);
        budget.copy(selected->parameters);
        budget.copy(selected->owner_semantics);
        const auto order = std::find(policy.ordered_hooks.begin(), policy.ordered_hooks.end(), grant.slot.hook_id);
        require(order != policy.ordered_hooks.end() && selected->ordered_hooks == policy.ordered_hooks &&
            v2::parameter_digest(selected->parameters) == grant.parameter_digest &&
            v2::owner_semantics_digest(selected->owner_semantics) == policy.owner_semantics_digest,
            "sealed_slot_mismatch", selected->pointer);
        const auto strings = v2::checked_sum(selected->pointer.size(), v2::checked_sum(grant.slot.owner_id.size(), grant.slot.hook_id.size()));
        budget.keep(v2::checked_product(strings, 8));
        budget.work(strings);
        program.hooks.push_back({ordinal, shape->contract, grant.slot, selected->pointer,
            static_cast<std::size_t>(order - policy.ordered_hooks.begin()), grant.parameter_digest,
            policy.owner_semantics_digest, grant.mutation_policy_digest, grant.abi_revision, shape->implementation_revision,
            shape->effects, {}, shape->provides, {}});
    }
    for (auto& hook : program.hooks) {
        const auto& policy = AuthorityInspection::policy(token, hook.grant_ordinal);
        budget.keep(v2::checked_product(policy.required_slots.size(), sizeof(std::size_t)));
        budget.work(0, v2::checked_product(v2::checked_sum(policy.required_slots.size(), policy.required_phases.size()), count));
        hook.inputs = fact_inputs(program, hook);
        hook.dependencies = slot_dependencies(token, program, hook);
        verify_symbol(token, hook);
    }
}

}
