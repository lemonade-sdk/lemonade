#include "backend_catalog_admission_internal.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace lemon::backends {

struct FrozenDefinitionSnapshot::Impl {
    std::shared_ptr<const DefinitionSourceSnapshot> sources;
    std::shared_ptr<const BuiltinExportRegistry> registry;
    DefinitionLimits limits;
};

namespace {
using namespace v2;

void require(bool condition, const std::string& code, const SourceLocation& source) {
    if (!condition) throw AuthorizationFailure{code, source};
}

std::string slot_key(const LogicalHookSlot& slot, Budget& budget) {
    const auto bytes = checked_sum(slot.owner_id.size(), checked_sum(slot.stage.size(), slot.hook_id.size()));
    budget.keep(checked_sum(checked_product(bytes, 3), sizeof(Json) * 12));
    budget.work(checked_product(bytes, 3), 12);
    return slot_json(slot).dump();
}

const Json* at_path(const Json& value, const std::vector<std::string>& tokens, Budget& budget) {
    const auto* current = &value;
    for (const auto& token : tokens) {
        budget.work(token.size(), 1);
        if (!current->is_object() || !current->contains(token)) return nullptr;
        current = &current->at(token);
    }
    return current;
}

const Json* policy_value(const Json& declaration, const DefinitionTarget& target, Budget& budget) {
    const auto* value = at_path(declaration, pointer_tokens(target.path, budget.limits), budget);
    if (value && target.match) {
        if (!value->is_array()) return nullptr;
        const auto field = target.match->field == RecordKey::Field::Id ? "id" : "name";
        const Json* found = nullptr;
        for (const auto& record : *value) {
            budget.work(0, 1);
            if (record.is_object() && record.value(field, Json()) == target.match->value) {
                require(!found, "ambiguous_authority", {{}, ""});
                found = &record;
            }
        }
        value = found ? at_path(*found, pointer_tokens(target.at, budget.limits), budget) : nullptr;
    }
    return value;
}

const Json* owner(const Json& declaration, const LogicalHookSlot& slot, Budget& budget) {
    if (slot.owner_kind == SlotOwnerKind::Operation)
        return policy_value(declaration, {"/operations", RecordKey{RecordKey::Field::Id, slot.owner_id}, ""}, budget);
    if (slot.owner_kind == SlotOwnerKind::Artifact)
        return policy_value(declaration, {"/artifacts/" + pointer_token(slot.owner_id), {}, ""}, budget);
    return &declaration;
}

bool hook_position(const std::vector<std::string>& path) {
    if (path.size() >= 2 && path[0] == "hooks") return true;
    if (path.size() >= 2 && path[0] == "install" && path[1] == "hook") return true;
    if (path.size() >= 3 && path[0] == "artifacts" && path[2] == "hook") return true;
    if (path.size() >= 3 && path[0] == "operations") {
        if (path[2] == "handler") return true;
        if (path.size() >= 4 && (path[2] == "request" || path[2] == "response") && path[3] == "hooks") return true;
    }
    return false;
}

void inspect_value(const Json& value, std::vector<std::string>& path, SourceLocation source, Budget& budget) {
    budget.work(source.pointer.size(), 1);
    require(!hook_position(path), "authored_hook_selection", source);
    if (!path.empty() && (path.size() == 1 || (path.size() == 2 && path[0] == "metadata")))
        for (const auto* field : {"origin", "authority", "grants", "compatibility", "trusted_compatibility", "builtin_launch"})
            require(path.back() != field, "forbidden_authority_control", source);
    if (path.size() <= 1 && value.is_object()) for (const auto* field : {"origin", "authority", "grants", "compatibility", "trusted_compatibility", "builtin_launch"})
        require(!value.contains(field), "forbidden_authority_control", {source.document, source.pointer + "/" + field});
    // Literal expression payloads do not introduce definition control syntax.
    const bool descend = path.empty() || (path.size() == 1 &&
        (path[0] == "hooks" || path[0] == "install" || path[0] == "artifacts" || path[0] == "operations")) ||
        (path.size() == 2 && (path[0] == "operations" || path[0] == "artifacts")) ||
        (path.size() == 3 && path[0] == "operations" && (path[2] == "request" || path[2] == "response"));
    if (!descend || !value.is_structured()) return;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto token = value.is_object() ? it.key() : std::to_string(it - value.begin());
        path.push_back(token);
        auto child = source;
        child.pointer = join_pointer(child.pointer, token, budget.limits);
        inspect_value(it.value(), path, std::move(child), budget);
        path.pop_back();
    }
}

void inspect_owner_value(const Json* value, std::vector<std::string>& path, const NodePtr& root,
    const BuiltinExportContract* contract, const SourceLocation& source, Budget& budget, std::string operation_id = "") {
    if (!contract) return;
    budget.work(0, 1);
    if (path.size() >= 2 && path[0] == "operations" && operation_id.empty()) {
        if (path.size() == 2 && value && value->is_object() && value->contains("id")) operation_id = value->at("id").get<std::string>();
        else {
            const auto record = effective_node(root, "/operations/" + pointer_token(path[1]), budget.limits);
            if (record && record->object.count("id")) operation_id = record->object.at("id")->scalar.get<std::string>();
        }
    }
    if (path.size() >= 3 && path[0] == "operations") for (const auto& policy : contract->owners)
        if (policy.id == operation_id)
            for (const auto& field : policy.sealed_fields)
                require(path[2] != field, "authored_owner_change", source);
    if (path.size() >= 3 && path[0] == "artifacts") for (const auto& policy : contract->slots)
        if (policy.slot.owner_kind == SlotOwnerKind::Artifact && policy.slot.owner_id == path[1])
            require(path[2] == "hook", "authored_owner_change", source);
    const bool descend = path.empty() || (path.size() == 1 && (path[0] == "operations" || path[0] == "artifacts")) ||
        (path.size() == 2 && (path[0] == "operations" || path[0] == "artifacts"));
    if (!value || !value->is_structured() || !descend) return;
    for (auto it = value->begin(); it != value->end(); ++it) {
        const auto name = value->is_object() ? it.key() : std::to_string(it - value->begin());
        path.push_back(name);
        inspect_owner_value(&it.value(), path, root, contract, source, budget, operation_id);
        path.pop_back();
    }
}

void erase_target(Json& declaration, const DefinitionTarget& target, Budget& budget) {
    auto tokens = pointer_tokens(target.path, budget.limits);
    Json* current = &declaration;
    if (target.match) {
        for (const auto& token : tokens) {
            if (!current->is_object() || !current->contains(token)) return;
            current = &current->at(token);
        }
        if (!current->is_array()) return;
        const auto field = target.match->field == RecordKey::Field::Id ? "id" : "name";
        Json* record = nullptr;
        for (auto& item : *current) if (item.value(field, Json()) == target.match->value) record = &item;
        if (!record) return;
        current = record;
        tokens = pointer_tokens(target.at, budget.limits);
    }
    if (tokens.empty()) return;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (!current->is_object() || !current->contains(tokens[i])) return;
        current = &current->at(tokens[i]);
    }
    if (current->is_object()) current->erase(tokens.back());
}

Json data_projection(const Json& declaration, const Json& base, const ExportSlotPolicy& policy,
    const BuiltinExportContract& contract, const Json& final_declaration, Budget& budget) {
    const auto metrics = measure(declaration, budget);
    budget.keep(checked_sum(metrics.bytes, checked_product(metrics.nodes, sizeof(Json))));
    budget.work(metrics.bytes, metrics.nodes);
    auto result = declaration;
    for (const auto* field : {"id", "recipe", "schema_version", "rationale", "hooks"}) result.erase(field);
    if (result.contains("install")) result["install"].erase("hook");
    for (const auto& rule : policy.mutable_inputs) erase_target(result, rule.target, budget);
    if (!final_declaration.value("hooks", Json::object()).contains("launch") && result.contains("launch")) {
        auto& order = result["launch"]["prepare_order"];
        order.erase(std::remove(order.begin(), order.end(), Json("hook")), order.end());
    }
    Json operations = Json::object();
    if (result.contains("operations")) for (const auto& source_operation : result["operations"]) {
        const auto operation_metrics = measure(source_operation, budget);
        budget.keep(checked_sum(operation_metrics.bytes, checked_product(operation_metrics.nodes, sizeof(Json))));
        auto operation = source_operation;
        const auto id = operation.at("id").get<std::string>();
        const LogicalHookSlot slot{SlotOwnerKind::Operation, id, "", ""};
        if (!owner(base, slot, budget)) continue;
        operation.erase("handler");
        for (const auto* stage : {"request", "response"}) if (operation.contains(stage)) {
            operation[stage].erase("hooks");
            if (operation[stage].empty()) operation.erase(stage);
        }
        operations[id] = std::move(operation);
    }
    result["operations"] = std::move(operations);
    if (result.contains("artifacts")) for (auto& artifact : result["artifacts"]) artifact.erase("hook");
    for (const auto& removed : contract.slots) if (removed.removal == SlotRemoval::WithOwner && !owner(final_declaration, removed.slot, budget)) {
        const auto& slot = removed.slot;
        if (slot.owner_kind == SlotOwnerKind::Operation) result["operations"].erase(slot.owner_id);
        if (slot.owner_kind == SlotOwnerKind::Artifact && result.contains("artifacts")) result["artifacts"].erase(slot.owner_id);
    }
    for (const auto& removed : contract.owners) if (removed.allow_removal &&
        !owner(final_declaration, {SlotOwnerKind::Operation, removed.id, "", ""}, budget)) result["operations"].erase(removed.id);
    return result;
}

bool same_source(const SourceLocation& left, const SourceLocation& right) {
    return key_string(left.document) == key_string(right.document) && left.pointer == right.pointer;
}

void verify_provenance(const NodePtr& before, const NodePtr& after, const SourceLocation& source, Budget& budget,
    const std::string& code = "hook_provenance_changed") {
    budget.work(0, 1);
    require(before && after && before->kind == after->kind && same_source(location(before->origin), location(after->origin)),
        code, source);
    require(before->object.size() == after->object.size() && before->array.size() == after->array.size(), code, source);
    for (const auto& [name, child] : before->object) {
        const auto found = after->object.find(name);
        require(found != after->object.end(), code, source);
        verify_provenance(child, found->second, source, budget, code);
    }
    for (std::size_t i = 0; i < before->array.size(); ++i) verify_provenance(before->array[i], after->array[i], source, budget, code);
}

void verify_owner_aliases(const Json& declaration, const Json& base, const ExportSlotPolicy& policy, Budget& budget, const SourceLocation& source) {
    const auto& slot = policy.slot;
    const auto* original = owner(base, slot, budget);
    if (slot.owner_kind == SlotOwnerKind::Artifact && declaration.contains("artifacts")) for (const auto& [name, artifact] : declaration["artifacts"].items()) {
        budget.work(0, 1);
        if (name == slot.owner_id) continue;
        require(artifact.value("role", Json()) != original->value("role", Json()), "protected_owner_recreated", source);
    }
}

NodePtr operation_owner_node(const NodePtr& root, const std::string& id, Budget& budget) {
    const auto operations = effective_node(root, "/operations", budget.limits);
    if (!operations) return {};
    for (const auto& operation : operations->array) {
        budget.work(0, 1);
        const auto found = operation->object.find("id");
        if (found != operation->object.end() && found->second->scalar == id) return operation;
    }
    return {};
}

void verify_operation_owners(const ResolvedDefinition& definition, const ResolvedDefinition& parent,
    const BuiltinExportContract& contract, const std::set<std::string>& selected,
    const NodePtr& parent_root, const NodePtr& current_root, Budget& budget) {
    const SourceLocation source{definition.key, "/operations"};
    std::set<std::string> surviving;
    for (const auto& policy : contract.owners) {
        const LogicalHookSlot slot{SlotOwnerKind::Operation, policy.id, "", ""};
        const auto* current = owner(definition.declaration, slot, budget);
        if (!current) continue;
        require(owner(parent.declaration, slot, budget) != nullptr, "owner_reintroduced", source);
        require(operation_owner_semantics(*current, budget) == policy.semantics, "owner_semantics_changed", source);
        const auto before = operation_owner_node(parent_root, policy.id, budget);
        const auto after = operation_owner_node(current_root, policy.id, budget);
        require(before && after, "owner_provenance_changed", source);
        for (const auto& field : policy.sealed_fields) {
            const auto a = before->object.find(field), b = after->object.find(field);
            require((a == before->object.end()) == (b == after->object.end()), "owner_provenance_changed", source);
            if (a != before->object.end()) verify_provenance(a->second, b->second, source, budget, "owner_provenance_changed");
        }
        for (const auto& dependency : policy.required_owners)
            require(owner(definition.declaration, {SlotOwnerKind::Operation, dependency, "", ""}, budget) != nullptr, "required_owner_removed", source);
        for (const auto* stage : {"request", "response", "handler"}) {
            const auto& chain = std::string_view(stage) == "request" ? policy.request_hooks :
                std::string_view(stage) == "response" ? policy.response_hooks : policy.handler_hooks;
            std::vector<std::string> expected;
            for (const auto& hook : chain) {
                const auto abi_stage = std::string_view(stage) != "handler" ? std::string(stage) :
                    current->at("delivery") == "streaming" ? "sink_operation" :
                    current->at("delivery") == "realtime" ? "realtime_operation" : "operation";
                if (selected.count(slot_key({SlotOwnerKind::Operation, policy.id, abi_stage, hook}, budget))) {
                    budget.keep(checked_sum(sizeof(std::string), hook.size()));
                    expected.push_back(hook);
                }
            }
            require(operation_hook_chain(*current, stage, budget) == expected, "owner_chain_changed", source);
        }
        budget.keep(checked_sum(sizeof(std::string), policy.id.size()));
        require(surviving.insert(policy.id).second, "ambiguous_authority", source);
    }
    for (const auto& operation : definition.declaration.at("operations")) {
        if (surviving.count(operation.at("id").get<std::string>())) continue;
        for (const auto& protected_owner : contract.owners)
            require(!operation_owners_collide(operation, protected_owner.semantics, budget), "protected_owner_recreated", source);
    }
}

}

namespace v2 {

void verify_slot_shape(const HookSelection& selection, const ExportSlotPolicy& policy,
    const SourceLocation& source, Budget& budget) {
    const auto* abi = find_hook_abi(selection.slot.hook_id);
    require(abi && abi->stage == selection.slot.stage && abi->revision == policy.hook_abi_revision &&
        (abi->operations.empty() || std::find(abi->operations.begin(), abi->operations.end(), selection.operation) != abi->operations.end()),
        "hook_abi_changed", source);
    require(digest("lemonade.backend-hook-parameters.v1\n", selection.parameters, budget) == policy.parameter_digest, "hook_parameters_changed", source);
    require(digest("lemonade.backend-hook-owner.v1\n", selection.owner_semantics, budget) == policy.owner_semantics_digest, "hook_owner_changed", source);
    require(selection.ordered_hooks == policy.ordered_hooks, "hook_order_changed", source);
}

std::vector<LogicalHookSlot> verify_surviving_grants(const ResolvedDefinition& definition,
    const ResolvedDefinition& parent, const ResolvedDefinition& base, const BuiltinExportContract* contract,
    const std::vector<LogicalHookSlot>& inherited_slots, const NodePtr& parent_root, const NodePtr& current_root,
    Budget& budget) {
    const auto& key = definition.key;
    const auto& limits = budget.limits;
    if (contract) for (const auto& policy : contract->owners)
        require(policy.allow_removal || owner(definition.declaration, {SlotOwnerKind::Operation, policy.id, "", ""}, budget),
            "mandatory_owner_removed", {key, "/operations"});
    const auto selections = hook_selections(definition.declaration, budget);
    std::set<std::string> selected;
    std::vector<LogicalHookSlot> surviving;
    for (const auto& selection : selections) {
        const SourceLocation source{key, selection.pointer};
        const auto name = slot_key(selection.slot, budget);
        require(selected.insert(name).second, "ambiguous_authority", source);
        const auto inherited = std::find_if(inherited_slots.begin(), inherited_slots.end(),
            [&](const auto& grant) { return slot_key(grant, budget) == name; });
        require(inherited != inherited_slots.end(), "grant_growth", source);
        require(contract != nullptr, "invalid_authority_graph", source);
        const auto& export_contract = *contract;
        const auto found_policy = std::find_if(export_contract.slots.begin(), export_contract.slots.end(),
            [&](const auto& policy) { return slot_key(policy.slot, budget) == name; });
        require(found_policy != export_contract.slots.end(), "invalid_authority_graph", source);
        const auto& policy = *found_policy;
        verify_slot_shape(selection, policy, source, budget);
        const auto parent_selections = hook_selections(parent.declaration, budget);
        const auto original = std::find_if(parent_selections.begin(), parent_selections.end(),
            [&](const auto& item) { return slot_key(item.slot, budget) == name; });
        require(original != parent_selections.end(), "grant_growth", source);
        verify_provenance(effective_node(parent_root, original->pointer, limits),
            effective_node(current_root, selection.pointer, limits), source, budget);
        for (const auto& target : policy.sealed_inputs) {
            const auto* before = policy_value(base.declaration, target, budget);
            const auto* after = policy_value(definition.declaration, target, budget);
            require(before && after && *before == *after, "sealed_input_changed", source);
        }
        for (const auto& rule : policy.mutable_inputs) require(accepts_mutable_value(rule,
            policy_value(definition.declaration, rule.target, budget), budget), "invalid_mutable_input", source);
        require(data_projection(definition.declaration, base.declaration, policy, export_contract, definition.declaration, budget) ==
            data_projection(base.declaration, base.declaration, policy, export_contract, definition.declaration, budget), "sealed_data_changed", source);
        surviving.push_back(selection.slot);
    }
    if (contract) for (const auto& policy : contract->slots) {
        const SourceLocation source{key, "/edits"};
        const auto exists = selected.count(slot_key(policy.slot, budget));
        if (!exists) {
            const auto* retained_owner = owner(definition.declaration, policy.slot, budget);
            require(policy.removal == SlotRemoval::Independent ||
                (policy.removal == SlotRemoval::WithOwner && !retained_owner), "mandatory_slot_removed", source);
        } else {
            for (const auto& required : policy.required_slots) require(selected.count(slot_key(required, budget)), "required_slot_removed", source);
            for (const auto& phase : policy.required_phases) require(definition.declaration.value("hooks", Json::object()).contains(phase), "required_phase_removed", source);
        }
        if (policy.removal != SlotRemoval::Independent && policy.slot.owner_kind == SlotOwnerKind::Artifact)
            verify_owner_aliases(definition.declaration, base.declaration, policy, budget, source);
    }
    if (contract) verify_operation_owners(definition, parent, *contract, selected, parent_root, current_root, budget);
    return surviving;

}

}

struct ResolvedAuthority::Impl {
    struct GrantIdentity {
        std::string base_id;
        std::uint32_t contract_major;
        std::string source_digest;
        std::string definition_hash;
        std::string dependency_digest;
        LogicalHookSlot slot;
        std::uint32_t abi_revision;
        std::string parameter_digest;
        std::string mutation_policy_digest;
    };
    std::shared_ptr<const FrozenDefinitionSnapshot> snapshot;
    std::shared_ptr<const ResolvedDefinition> resolved;
    std::shared_ptr<const ResolvedDefinition> base;
    const BuiltinExportContract* contract = nullptr;
    std::string registry_digest;
    std::uint32_t authorization_revision = 2;
    std::vector<GrantIdentity> grants;
};

FrozenDefinitionSnapshotResult BuiltinCatalogLoader::freeze_sources(const std::vector<DefinitionSourceView>& sources,
    std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits, std::size_t* retained) {
    FrozenDefinitionSnapshotResult result;
    const auto input = make_v2_source_snapshot_accounted(sources, limits, retained);
    result.diagnostics = input.diagnostics;
    if (!input.snapshot) return result;
    const auto audit = audit_export_contracts(registry->exports(), input.snapshot, installed_hook_abis(), registry->digest(), limits);
    result.diagnostics = audit;
    if (!audit.entries.empty() || audit.truncated) return result;
    auto impl = std::make_shared<FrozenDefinitionSnapshot::Impl>();
    impl->sources = input.snapshot;
    impl->registry = std::move(registry);
    impl->limits = limits;
    auto snapshot = std::shared_ptr<FrozenDefinitionSnapshot>(new FrozenDefinitionSnapshot());
    snapshot->impl_ = std::move(impl);
    result.snapshot = std::move(snapshot);
    return result;
}

FrozenDefinitionSnapshotResult BuiltinCatalogLoader::freeze(const std::vector<DefinitionSourceView>& external_sources,
    const DefinitionLimits& limits) {
    for (const auto& source : external_sources) if (source.key.space != DefinitionNamespace::External || source.key.contract != 0) {
        FrozenDefinitionSnapshotResult result;
        if (limits.diagnostics) result.diagnostics.entries.push_back({"origin_promotion", source.key.id, "", "External intake requires external origin", {}, {}, {}});
        else result.diagnostics.truncated = true;
        return result;
    }
    return freeze_sources(external_sources, std::make_shared<const BuiltinExportRegistry>(installed_exports()), limits);
}

FrozenDefinitionSnapshotResult BuiltinCatalogLoader::compose(std::shared_ptr<const FrozenDefinitionSnapshot> builtins,
    const std::vector<DefinitionSourceView>& external_sources, const DefinitionLimits& limits, std::size_t& retained) {
    std::vector<DefinitionSourceView> sources;
    charge(retained, checked_product(checked_sum(external_sources.size(), builtins->impl_->sources->sources.size()),
        sizeof(DefinitionSourceView) + sizeof(DefinitionKey)), limits.retained_logical_bytes);
    for (const auto& [key, source] : builtins->impl_->sources->sources)
        if (source.key.space == DefinitionNamespace::Builtin) sources.push_back({source.key, source.bytes, ""});
    for (const auto& source : external_sources) {
        require(source.key.space == DefinitionNamespace::External && source.key.contract == 0, "origin_promotion", {source.key, ""});
        sources.push_back(source);
    }
    return freeze_sources(sources, builtins->impl_->registry, limits, &retained);
}

#ifdef LEMON_BACKEND_EXPORT_TESTING
FrozenDefinitionSnapshotResult BuiltinCatalogLoader::test_freeze(const std::vector<DefinitionSourceView>& sources,
    std::shared_ptr<const BuiltinExportRegistry> registry, const DefinitionLimits& limits) {
    return freeze_sources(sources, std::move(registry), limits);
}
#endif

FrozenDefinitionSnapshotResult load_v2_authority_snapshot(const std::vector<DefinitionSourceView>& external_sources,
    const DefinitionLimits& limits) {
    return BuiltinCatalogLoader::freeze(external_sources, limits);
}

#ifdef LEMON_BACKEND_EXPORT_TESTING
std::shared_ptr<const FrozenBuiltinCatalog> CatalogAdmissionBuilder::test_builtins(
    std::shared_ptr<const FrozenDefinitionSnapshot> authority, const std::vector<DefinitionSourceView>& sources) {
    auto value = std::shared_ptr<FrozenBuiltinCatalog>(new FrozenBuiltinCatalog());
    value->authority_ = std::move(authority);
    value->identity_ = value->authority_->impl_->registry->digest();
    std::set<std::string> seen;
    for (const auto& source : sources) if (seen.insert(source.key.id).second) {
        value->ids_.insert(source.key.id);
        ++value->definitions_;
        value->source_bytes_ = checked_sum(value->source_bytes_, source.bytes.size());
    }
    value->logical_bytes_ = value->authority_->impl_->sources->logical_bytes;
    const DefinitionLimits limits;
    Budget budget{limits, {}, value->logical_bytes_};
    budget.keep(sizeof(FrozenBuiltinCatalog) + sizeof(FrozenDefinitionSnapshot::Impl));
    for (const auto& contract : value->authority_->impl_->registry->exports()) {
        const auto policy = export_policy_json(contract);
        const auto metrics = measure(policy, budget);
        budget.keep(checked_sum(metrics.bytes, checked_product(metrics.nodes, sizeof(Json) * 4)));
    }
    return value;
}

#endif

std::string CatalogAdmissionBuilder::authority_identity(const AuthorizedDefinition& definition) {
    const auto& authority = *definition.authority_.impl_;
    Json grants = Json::array();
    for (const auto& grant : authority.grants) grants.push_back({{"builtin", grant.base_id},
        {"contract", grant.contract_major}, {"slot", slot_json(grant.slot)}});
    return identity("lemonade.backend-authorization.v1\n", {{"revision", authority.authorization_revision},
        {"registry", authority.registry_digest}, {"definition", authority.resolved->definition_hash},
        {"dependencies", authority.resolved->dependency_digest}, {"grants", grants}});
}

const ResolvedDefinition& AuthorizedDefinition::definition() const noexcept { return *definition_; }
const ResolvedAuthority& AuthorizedDefinition::authority() const noexcept { return authority_; }

DefinitionAuthorizer::DefinitionAuthorizer(std::shared_ptr<const FrozenDefinitionSnapshot> snapshot) : snapshot_(std::move(snapshot)) {}

DefinitionAuthorizationResult DefinitionAuthorizer::authorize(const DefinitionKey& leaf) const {
    return authorize_accounted(leaf, nullptr);
}

DefinitionAuthorizationResult DefinitionAuthorizer::authorize_accounted(const DefinitionKey& leaf, std::size_t* shared_retained) const {
    DefinitionAuthorizationResult result;
    const auto limits = snapshot_ ? snapshot_->impl_->limits : DefinitionLimits{};
    const auto error = [&](const std::string& code, const SourceLocation& source) {
        if (result.diagnostics.entries.size() < limits.diagnostics)
            result.diagnostics.entries.push_back({code, source.document.id, source.pointer, "Definition authorization failed", {}, source, {}});
        else result.diagnostics.truncated = true;
    };
    try {
        require(snapshot_ != nullptr, "invalid_authority_snapshot", {leaf, ""});
        require(leaf.space == DefinitionNamespace::External && leaf.contract == 0, "origin_promotion", {leaf, ""});
        const auto& frozen = *snapshot_->impl_;
        DefinitionResolver resolver(frozen.sources, limits, shared_retained);
        const auto resolved = resolver.resolve(leaf);
        if (!resolved.definition) {
            auto current = leaf;
            std::set<std::string> seen;
            while (seen.insert(key_string(current)).second && seen.size() <= limits.chain_documents) {
                const auto found = frozen.sources->sources.find(key_string(current));
                if (found == frozen.sources->sources.end() || !found->second.header.contains("extends")) break;
                const auto& dependency = found->second.header["extends"];
                if (dependency.contains("builtin")) {
                    require(frozen.registry->find(dependency["builtin"].get<std::string>(), dependency["contract"].get<std::uint32_t>()) != nullptr,
                        std::any_of(frozen.registry->exports().begin(), frozen.registry->exports().end(), [&](const auto& contract) {
                            return contract.key.id == dependency["builtin"];
                        }) ? "contract_mismatch" : "unexported_builtin", {current, "/extends"});
                    break;
                }
                current = {DefinitionNamespace::External, dependency["external"], 0};
            }
            result.diagnostics = resolved.diagnostics;
            return result;
        }
        result.resolved = true;
        std::size_t retained = resolved.definition->metrics.retained_logical_bytes;
        Budget budget{limits, resolved.definition->metrics, shared_retained ? *shared_retained : retained};
        std::vector<DefinitionKey> chain;
        for (const auto& dependency : resolved.definition->dependencies) chain.push_back(dependency.document);
        chain.push_back(leaf);
        std::shared_ptr<const ResolvedDefinition> parent;
        std::shared_ptr<ResolvedAuthority::Impl> authority;
        for (std::size_t index = 0; index < chain.size(); ++index) {
            const auto& key = chain[index];
            const auto layer = key_string(key) == key_string(leaf) ? resolved : resolver.resolve(key);
            require(layer.definition != nullptr, "invalid_authority_graph", {key, ""});
            budget.keep(sizeof(ResolvedAuthority::Impl));
            const auto& definition = *layer.definition;
            const auto& record = frozen.sources->sources.at(key_string(key));
            if (key.space == DefinitionNamespace::Builtin) {
                if (index + 1 < chain.size() && chain[index + 1].space == DefinitionNamespace::Builtin) {
                    parent = layer.definition;
                    continue;
                }
                const auto* contract = frozen.registry->find(key.id, key.contract);
                require(contract != nullptr, "unexported_builtin", {key, ""});
                authority = std::make_shared<ResolvedAuthority::Impl>();
                authority->base = layer.definition;
                authority->contract = contract;
                for (const auto& policy : contract->slots) {
                    slot_key(policy.slot, budget);
                    budget.keep(checked_sum(sizeof(ResolvedAuthority::Impl::GrantIdentity), checked_sum(key.id.size(), 512)));
                    authority->grants.push_back({key.id, key.contract, definition.source_digest, definition.definition_hash,
                        definition.dependency_digest, policy.slot, policy.hook_abi_revision, policy.parameter_digest, policy.mutation_policy_digest});
                }
                parent = layer.definition;
                continue;
            }
            if (!authority) authority = std::make_shared<ResolvedAuthority::Impl>();
            budget.work(record.bytes.size(), record.nodes);
            budget.keep(checked_sum(record.bytes.size(), checked_product(record.nodes, sizeof(Json))));
            const auto envelope = Json::parse(record.bytes);
            LayerState inspection;
            if (parent) inspection.root = parent->provenance->impl_->root;
            apply_layer(inspection, envelope, key, budget,
                [&](const NodePtr& root, const std::string& pointer, DefinitionEditKind, const Json* value, const SourceLocation& source) {
                    auto path = pointer_tokens(pointer, limits);
                    if (value) inspect_value(*value, path, source, budget);
                    inspect_owner_value(value, path, root, authority->contract, source, budget);
                });
            std::vector<LogicalHookSlot> inherited;
            for (const auto& grant : authority->grants) {
                const auto bytes = measure(slot_json(grant.slot), budget).bytes;
                budget.keep(checked_sum(sizeof(ResolvedAuthority::Impl::GrantIdentity) * 2, checked_sum(bytes * 2, 1024)));
                inherited.push_back(grant.slot);
            }
            const auto slots = verify_surviving_grants(definition, parent ? *parent : definition,
                authority->base ? *authority->base : definition, authority->contract, inherited,
                parent ? parent->provenance->impl_->root : NodePtr{}, definition.provenance->impl_->root, budget);
            std::vector<ResolvedAuthority::Impl::GrantIdentity> surviving;
            for (const auto& slot : slots) for (const auto& grant : authority->grants)
                if (slot_key(slot, budget) == slot_key(grant.slot, budget)) surviving.push_back(grant);
            authority->grants = std::move(surviving);
            parent = layer.definition;
        }
        authority->snapshot = snapshot_;
        authority->resolved = resolved.definition;
        authority->registry_digest = frozen.registry->digest();
        budget.keep(sizeof(AuthorizedDefinition) + sizeof(ResolvedAuthority));
        auto authorized = std::shared_ptr<AuthorizedDefinition>(new AuthorizedDefinition());
        authorized->definition_ = resolved.definition;
        authorized->authority_.impl_ = std::move(authority);
        result.definition = std::move(authorized);
    } catch (const AuthorizationFailure& failure) { error(failure.code, failure.source); }
    catch (const LimitExceeded&) { error("limit_exceeded", {leaf, ""}); }
    catch (const std::exception&) { error("invalid_authority_graph", {leaf, ""}); }
    return result;
}

}
