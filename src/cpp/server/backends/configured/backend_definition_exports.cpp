#include "backend_definition_exports_internal.h"
#include "backend_definition_policy_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>

namespace lemon::backends::v2 {
namespace {

Json target_json(const DefinitionTarget& target) {
    Json result = {{"path", target.path}, {"at", target.at}, {"match", nullptr}};
    if (target.match) result["match"] = {{target.match->field == RecordKey::Field::Id ? "id" : "name", target.match->value}};
    return result;
}

Json mutable_rule_json(const MutableDataRule& rule) {
    return {{"target", target_json(rule.target)}, {"type", static_cast<unsigned>(rule.type)},
        {"allow_absent", rule.allow_absent}, {"minimum", rule.minimum ? Json(*rule.minimum) : Json()},
        {"maximum", rule.maximum ? Json(*rule.maximum) : Json()}, {"maximum_length", rule.maximum_length},
        {"values", rule.values}, {"schema_fragment", rule.schema_fragment}, {"reference_roots", rule.reference_roots}};
}

bool has_digest(std::string_view text) {
    return text.size() == 71 && text.substr(0, 7) == "sha256:" &&
        std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string slot_key(const LogicalHookSlot& slot) { return slot_json(slot).dump(); }

bool valid_target(const DefinitionTarget& target, const DefinitionLimits& limits) {
    if (target.path.empty() || (!target.match && !target.at.empty())) return false;
    pointer_tokens(target.path, limits);
    pointer_tokens(target.at, limits);
    if (target.match) {
        const auto field = record_field(target.path);
        if (!field || *field != target.match->field || target.match->value.empty()) return false;
    }
    return true;
}

bool targets_overlap(const DefinitionTarget& left, const DefinitionTarget& right, const DefinitionLimits& limits) {
    const auto route = [&](const DefinitionTarget& target) {
        std::vector<std::pair<bool, std::string>> result;
        for (const auto& token : pointer_tokens(target.path, limits)) result.emplace_back(false, token);
        if (target.match) {
            result.emplace_back(true, (target.match->field == RecordKey::Field::Id ? "id:" : "name:") + target.match->value);
            for (const auto& token : pointer_tokens(target.at, limits)) result.emplace_back(false, token);
        }
        return result;
    };
    const auto a = route(left), b = route(right);
    return std::equal(a.begin(), a.begin() + std::min(a.size(), b.size()), b.begin());
}

bool changes_owner_semantics(const MutableDataRule& rule, const std::vector<HookSelection>& selections,
    const std::vector<OperationOwnerPolicy>& owners, const DefinitionLimits& limits) {
    for (const auto& owner : owners) for (const auto& field : owner.sealed_fields)
        if (targets_overlap(rule.target, {"/operations", RecordKey{RecordKey::Field::Id, owner.id}, "/" + field}, limits)) return true;
    for (const auto& selection : selections) {
        if (selection.slot.owner_kind == SlotOwnerKind::Artifact &&
            targets_overlap(rule.target, {selection.owner_pointer, {}, ""}, limits)) return true;
        if (selection.slot.owner_kind == SlotOwnerKind::Operation) for (const auto& [field, value] : selection.owner_semantics.items()) {
            const DefinitionTarget target{"/operations", RecordKey{RecordKey::Field::Id, selection.slot.owner_id}, "/" + pointer_token(field)};
            if (targets_overlap(rule.target, target, limits)) return true;
        }
        if (selection.pointer == "/install/hook" && targets_overlap(rule.target, {"/install/strategy", {}, ""}, limits)) return true;
    }
    return false;
}

bool known_endpoint(const Json& operation, Budget& budget) {
    if (!operation.contains("transport")) return false;
    const auto& transport = operation.at("transport");
    for (const auto* field : {"kind", "method", "path", "port"})
        if (!transport.contains(field) || !transport.at(field).is_string()) return false;
    const auto& kind = transport.at("kind").get_ref<const std::string&>();
    if (kind != "http_json" && kind != "http_sse" && kind != "http_bytes" && kind != "multipart") return false;
    const auto& method = transport.at("method").get_ref<const std::string&>();
    if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" &&
        method != "DELETE" && method != "HEAD" && method != "OPTIONS") return false;
    const auto& path = transport.at("path").get_ref<const std::string&>();
    const auto& port = transport.at("port").get_ref<const std::string&>();
    budget.work(checked_sum(path.size(), port.size()), 1);
    if (path.empty() || path.front() != '/' || port.empty()) return false;
    const auto token = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    if (!std::all_of(port.begin(), port.end(), token)) return false;
    if (!std::all_of(path.begin(), path.end(), [&](char c) { return token(c) || c == '/' || c == '.' || c == '~'; })) return false;
    if (path.find("//") != std::string::npos) return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto length = (end == std::string::npos ? path.size() : end) - start;
        if ((length == 1 && path[start] == '.') || (length == 2 && path[start] == '.' && path[start + 1] == '.')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

NodePtr policy_target(const NodePtr& root, const DefinitionTarget& target, const DefinitionLimits& limits) {
    try { return target_node(root, target, limits); }
    catch (const EditFailure& failure) {
        if (failure.code == "missing_target") return {};
        throw;
    }
}

bool valid_mutable_rule(const MutableDataRule& rule, const DefinitionLimits& limits) {
    if (!valid_target(rule.target, limits) || !rule.values.is_array()) return false;
    const auto type = rule.type;
    if (type != MutableValueType::Boolean && type != MutableValueType::Integer &&
        type != MutableValueType::Number && type != MutableValueType::String && type != MutableValueType::Expression) return false;
    if ((rule.minimum && !std::isfinite(*rule.minimum)) || (rule.maximum && !std::isfinite(*rule.maximum)) ||
        (rule.minimum && rule.maximum && *rule.minimum > *rule.maximum)) return false;
    if ((rule.minimum || rule.maximum) && type != MutableValueType::Integer && type != MutableValueType::Number) return false;
    if (type == MutableValueType::Integer) for (const auto& bound : {rule.minimum, rule.maximum})
        if (bound && (std::abs(*bound) > 9007199254740991.0 || std::floor(*bound) != *bound)) return false;
    if (rule.maximum_length && type != MutableValueType::String) return false;
    if (type == MutableValueType::Expression) {
        if (rule.schema_fragment != "Expression" || rule.reference_roots.empty() || !rule.values.empty()) return false;
        for (const auto& root : rule.reference_roots) if (root != "model" && root != "request" && root != "options" && root != "runtime" && root != "system") return false;
    } else if (!rule.schema_fragment.empty() || !rule.reference_roots.empty()) return false;
    for (const auto& value : rule.values) {
        if (type == MutableValueType::Boolean ? !value.is_boolean() :
            type == MutableValueType::Integer ? !value.is_number_integer() :
            type == MutableValueType::Number ? !value.is_number() : !value.is_string()) return false;
        if (value.is_number() && !std::isfinite(value.get<double>())) return false;
        if (value.is_number() && ((rule.minimum && value < *rule.minimum) || (rule.maximum && value > *rule.maximum))) return false;
        if (value.is_string() && rule.maximum_length && value.get_ref<const std::string&>().size() > rule.maximum_length) return false;
    }
    return true;
}

void audit_owner_graph(const BuiltinExportContract& contract, const Json& declaration, Budget& budget,
    const std::function<void(const std::string&, const std::string&, const std::string&)>& error) {
    const auto fail = [&](const std::string& code) { error(contract.key.id, code, "/operations"); };
    if (owner_graph_digest(contract.owners) != contract.owner_graph_digest) fail("owner_graph_digest_mismatch");
    const auto& operations = declaration.at("operations");
    std::set<std::string> covered;
    for (const auto& owner : contract.owners) {
        budget.work(owner.id.size(), 1);
        budget.keep(checked_sum(owner.id.size(), sizeof(OperationOwnerPolicy)));
        if (!covered.insert(owner.id).second) fail("duplicate_operation_owner");
        const auto original = std::find_if(operations.begin(), operations.end(), [&](const auto& operation) {
            budget.work(0, 1);
            return operation.at("id") == owner.id;
        });
        if (original == operations.end()) { fail("missing_operation_owner"); continue; }
        if (owner.semantics != operation_owner_semantics(*original, budget)) fail("operation_owner_semantics_mismatch");
        if (owner.sealed_fields != operation_owner_fields()) fail("invalid_owner_fields");
        if (owner.request_hooks != operation_hook_chain(*original, "request", budget) ||
            owner.response_hooks != operation_hook_chain(*original, "response", budget) ||
            owner.handler_hooks != operation_hook_chain(*original, "handler", budget)) fail("owner_chain_mismatch");
        std::vector<std::string> collisions;
        for (const auto& sibling : operations) {
            budget.work(0, 1);
            if (sibling.at("id") != owner.id && operation_owners_collide(*original, sibling, budget)) {
                const auto& id = sibling.at("id").get_ref<const std::string&>();
                budget.keep(checked_sum(sizeof(std::string), id.size()));
                collisions.push_back(id);
            }
        }
        if (owner.collisions != collisions) fail("owner_collisions_mismatch");
        std::set<std::string> dependencies;
        for (const auto& dependency : owner.required_owners) {
            budget.work(dependency.size(), 1);
            budget.keep(checked_sum(dependency.size(), sizeof(std::string)));
            if (dependency == owner.id || !dependencies.insert(dependency).second ||
                std::none_of(contract.owners.begin(), contract.owners.end(), [&](const auto& item) { return item.id == dependency; }))
                fail("invalid_owner_dependency");
        }
        for (const auto& slot : contract.slots) if (slot.slot.owner_kind == SlotOwnerKind::Operation && slot.slot.owner_id == owner.id &&
            ((slot.removal == SlotRemoval::Forbidden && owner.allow_removal) || (slot.removal == SlotRemoval::WithOwner && !owner.allow_removal)))
            fail("invalid_owner_removal");
    }
    for (const auto& operation : operations) if (!covered.count(operation.at("id").get<std::string>())) fail("uncovered_operation_owner");
    std::set<std::string> active, complete;
    const auto visit = [&](const auto& self, const OperationOwnerPolicy& owner) -> bool {
        budget.work(0, 1);
        if (complete.count(owner.id)) return true;
        budget.keep(checked_product(checked_sum(sizeof(std::string), owner.id.size()), 2));
        if (!active.insert(owner.id).second) return false;
        for (const auto& dependency : owner.required_owners) {
            const auto required = std::find_if(contract.owners.begin(), contract.owners.end(), [&](const auto& item) { return item.id == dependency; });
            if (required != contract.owners.end() && !self(self, *required)) return false;
        }
        active.erase(owner.id);
        complete.insert(owner.id);
        return true;
    };
    for (const auto& owner : contract.owners) if (!visit(visit, owner)) { fail("owner_dependency_cycle"); break; }
}

}

std::string identity(std::string_view domain, const Json& value) {
    const DefinitionLimits limits;
    std::size_t retained = 0;
    Budget budget{limits, {}, retained};
    return digest(domain, value, budget);
}

Json slot_json(const LogicalHookSlot& slot) {
    return {{"owner_kind", static_cast<unsigned>(slot.owner_kind)}, {"owner_id", slot.owner_id},
        {"stage", slot.stage}, {"hook_id", slot.hook_id}};
}

Json slot_policy_json(const ExportSlotPolicy& policy) {
    auto dependencies = Json::array(), sealed = Json::array(), mutable_fields = Json::array(), effects = Json::array();
    for (const auto& slot : policy.required_slots) dependencies.push_back(slot_json(slot));
    for (const auto& target : policy.sealed_inputs) sealed.push_back(target_json(target));
    for (const auto& rule : policy.mutable_inputs) mutable_fields.push_back(mutable_rule_json(rule));
    for (const auto effect : policy.effects) effects.push_back(static_cast<unsigned>(effect));
    Json result = {{"slot", slot_json(policy.slot)}, {"abi", policy.hook_abi_revision}, {"parameters", policy.parameter_digest},
        {"removal", static_cast<unsigned>(policy.removal)}, {"dependencies", dependencies}, {"sealed_inputs", sealed},
        {"mutable_inputs", mutable_fields}, {"required_phases", policy.required_phases}, {"effects", effects},
        {"owner_semantics", policy.owner_semantics_digest}, {"ordered_hooks", policy.ordered_hooks}};
    if (!policy.mutable_fragments.empty()) result["mutable_fragments"] = fragment_rules_json(policy.mutable_fragments);
    return result;
}

const std::vector<std::string>& operation_owner_fields() {
    static const std::vector<std::string> fields = {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"};
    return fields;
}

Json operation_owner_semantics(const Json& operation, Budget& budget) {
    Json result = Json::object();
    for (const auto& field : operation_owner_fields()) if (operation.contains(field)) {
        const auto metrics = measure(operation.at(field), budget);
        budget.keep(checked_sum(field.size(), checked_sum(metrics.bytes, checked_product(metrics.nodes, sizeof(Json)))));
        result[field] = operation.at(field);
    }
    return result;
}

std::vector<std::string> operation_hook_chain(const Json& operation, std::string_view stage, Budget& budget) {
    std::vector<std::string> result;
    const auto append = [&](const Json& hook) {
        const auto& id = hook.at("id").get_ref<const std::string&>();
        budget.work(id.size(), 1);
        budget.keep(checked_sum(sizeof(std::string), id.size()));
        result.push_back(id);
    };
    if (stage == "handler") {
        if (operation.contains("handler")) append(operation.at("handler"));
    } else if (operation.contains(stage) && operation.at(stage).contains("hooks"))
        for (const auto& hook : operation.at(stage).at("hooks")) append(hook);
    return result;
}

bool operation_owners_collide(const Json& left, const Json& right, Budget& budget) {
    budget.work(0, 1);
    if (left.at("operation") == right.at("operation")) return true;
    if (!known_endpoint(left, budget) || !known_endpoint(right, budget)) return true;
    const auto& a = left.at("transport");
    const auto& b = right.at("transport");
    return a.at("method") == b.at("method") && a.at("path") == b.at("path") && a.at("port") == b.at("port");
}

Json owner_graph_json(const std::vector<OperationOwnerPolicy>& owners) {
    auto result = Json::array();
    for (const auto& owner : owners) result.push_back({{"id", owner.id}, {"semantics", owner.semantics},
        {"request_hooks", owner.request_hooks}, {"response_hooks", owner.response_hooks}, {"handler_hooks", owner.handler_hooks},
        {"allow_removal", owner.allow_removal}, {"required_owners", owner.required_owners},
        {"sealed_fields", owner.sealed_fields}, {"collisions", owner.collisions}});
    return result;
}

std::string owner_graph_digest(const std::vector<OperationOwnerPolicy>& owners) {
    return identity("lemonade.backend-operation-owners.v1\n", owner_graph_json(owners));
}

Json export_policy_json(const BuiltinExportContract& contract) {
    auto slots = Json::array();
    for (const auto& policy : contract.slots) {
        auto value = slot_policy_json(policy);
        value["mutation_policy_digest"] = policy.mutation_policy_digest;
        slots.push_back(std::move(value));
    }
    Json result = {{"builtin", contract.key.id}, {"contract", contract.key.contract}, {"policy_revision", contract.policy_revision},
        {"slots", slots}, {"owners", owner_graph_json(contract.owners)}, {"owner_graph_digest", contract.owner_graph_digest}};
    if (!contract.mutable_fragments.empty()) result["mutable_fragments"] = fragment_rules_json(contract.mutable_fragments);
    return result;
}

Json hook_abi_json(const std::vector<HookAbiDescriptor>& descriptors) {
    auto rows = Json::array();
    for (const auto& descriptor : descriptors) rows.push_back({{"id", descriptor.id}, {"stage", descriptor.stage},
        {"revision", descriptor.revision}, {"implementation_revision", descriptor.implementation_revision}, {"operations", descriptor.operations}});
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.at("id") < right.at("id"); });
    return rows;
}

std::string parameter_digest(const Json& parameters) { return identity("lemonade.backend-hook-parameters.v1\n", parameters); }
std::string owner_semantics_digest(const Json& semantics) { return identity("lemonade.backend-hook-owner.v1\n", semantics); }
std::string mutation_policy_digest(const ExportSlotPolicy& policy) { return identity("lemonade.backend-hook-mutation-policy.v1\n", slot_policy_json(policy)); }
std::string export_policy_digest(const BuiltinExportContract& contract) { return identity("lemonade.backend-export-policy.v1\n", export_policy_json(contract)); }

std::string export_registry_digest(const std::vector<BuiltinExportContract>& contracts, const std::vector<HookAbiDescriptor>& abis) {
    auto rows = Json::array();
    for (const auto& contract : contracts) rows.push_back({{"builtin", contract.key.id}, {"contract", contract.key.contract},
        {"source", contract.expected_source_digest}, {"definition", contract.expected_definition_hash},
        {"dependencies", contract.expected_dependency_digest}, {"policy_revision", contract.policy_revision}, {"policy", contract.policy_digest}});
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return std::make_pair(left.at("builtin"), left.at("contract")) < std::make_pair(right.at("builtin"), right.at("contract"));
    });
    return identity("lemonade.backend-export-registry.v1\n", {{"exports", rows}, {"hook_abis", hook_abi_json(abis)}});
}

bool accepts_mutable_value(const MutableDataRule& rule, const Json* value, Budget& budget) {
    if (!value) return rule.allow_absent;
    budget.work(0, 1);
    if (rule.type == MutableValueType::Expression) {
        const auto checked = check_fragment(*value, rule.schema_fragment, budget);
        if (!checked.entries.empty() || checked.truncated) return false;
        const auto visit = [&](const auto& self, const Json& expression) -> bool {
            budget.work(0, 1);
            if (!expression.is_structured()) return true;
            if (expression.is_object()) {
                if (expression.contains("literal")) return true;
                if (expression.contains("option") && std::find(rule.reference_roots.begin(), rule.reference_roots.end(), "options") == rule.reference_roots.end()) return false;
                if (expression.contains("ref")) {
                    const auto parts = pointer_tokens(expression["ref"].get<std::string>(), budget.limits);
                    if (parts.empty() || std::find(rule.reference_roots.begin(), rule.reference_roots.end(), parts.front()) == rule.reference_roots.end()) return false;
                }
            }
            for (auto child = expression.begin(); child != expression.end(); ++child) {
                if (expression.is_object() && (child.key() == "ref" || child.key() == "option" || child.key() == "default" || child.key() == "type")) continue;
                if (!self(self, child.value())) return false;
            }
            return true;
        };
        return visit(visit, *value);
    }
    if (rule.type == MutableValueType::Boolean ? !value->is_boolean() :
        rule.type == MutableValueType::Integer ? !value->is_number_integer() :
        rule.type == MutableValueType::Number ? !value->is_number() : !value->is_string()) return false;
    if (value->is_number() && (!std::isfinite(value->get<double>()) ||
        (rule.minimum && *value < *rule.minimum) || (rule.maximum && *value > *rule.maximum))) return false;
    if (value->is_string() && rule.maximum_length && value->get_ref<const std::string&>().size() > rule.maximum_length) return false;
    return rule.values.empty() || std::find(rule.values.begin(), rule.values.end(), *value) != rule.values.end();
}

std::vector<HookSelection> hook_selections(const Json& declaration, Budget& budget) {
    std::vector<HookSelection> result;
    const auto append = [&](const Json& hook, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, const std::string& pointer, const std::string& owner_pointer,
        const std::string& operation, const Json& semantics, const std::vector<std::string>& ordered) {
        budget.work(pointer.size(), 1);
        const auto semantics_metrics = measure(semantics, budget);
        budget.keep(checked_sum(semantics_metrics.bytes, checked_product(semantics_metrics.nodes, sizeof(Json))));
        for (const auto& id : ordered) { budget.keep(checked_sum(sizeof(std::string), id.size())); budget.work(id.size(), 1); }
        const auto hook_metrics = measure(hook, budget);
        budget.keep(checked_sum(sizeof(HookSelection), checked_sum(hook_metrics.bytes, checked_product(hook_metrics.nodes, sizeof(Json)))));
        for (const auto& text : {pointer, owner_pointer, operation, owner, stage}) budget.keep(text.size());
        result.push_back({{kind, owner, stage, hook.at("id")}, pointer, owner_pointer, operation,
            hook.value("params", Json::object()), semantics, ordered});
    };
    if (declaration.contains("hooks")) for (const auto& [stage, hook] : declaration["hooks"].items())
        append(hook, SlotOwnerKind::Root, stage, stage, "/hooks/" + pointer_token(stage), "/hooks/" + pointer_token(stage), "", Json{{"slot", stage}}, {hook.at("id")});
    if (declaration.contains("install") && declaration["install"].contains("hook"))
        append(declaration["install"]["hook"], SlotOwnerKind::Root, "install", "install", "/install/hook", "/install", "", Json{{"strategy", declaration["install"].at("strategy")}}, {declaration["install"]["hook"].at("id")});
    if (declaration.contains("artifacts")) for (const auto& [name, artifact] : declaration["artifacts"].items()) if (artifact.contains("hook")) {
        const auto artifact_metrics = measure(artifact, budget);
        budget.keep(checked_sum(artifact_metrics.bytes, checked_product(artifact_metrics.nodes, sizeof(Json))));
        auto semantics = artifact;
        semantics.erase("hook");
        append(artifact["hook"], SlotOwnerKind::Artifact, name, "artifacts", "/artifacts/" + pointer_token(name) + "/hook",
            "/artifacts/" + pointer_token(name), "", semantics, {artifact["hook"].at("id")});
    }
    if (declaration.contains("operations")) for (std::size_t index = 0; index < declaration["operations"].size(); ++index) {
        const auto& operation = declaration["operations"][index];
        const auto path = "/operations/" + std::to_string(index);
        const auto operation_metrics = measure(operation, budget);
        budget.keep(checked_sum(operation_metrics.bytes, checked_product(operation_metrics.nodes, sizeof(Json))));
        Json semantics = Json::object();
        for (const auto* key : {"id", "operation", "delivery", "model_modes", "transport", "formats", "format_metadata"})
            if (operation.contains(key)) semantics[key] = operation[key];
        const auto name = operation.at("id").get<std::string>();
        const auto method = operation.at("operation").get<std::string>();
        if (operation.contains("handler")) {
            const auto stage = operation.at("delivery") == "streaming" ? "sink_operation" :
                operation.at("delivery") == "realtime" ? "realtime_operation" : "operation";
            append(operation["handler"], SlotOwnerKind::Operation, name, stage, path + "/handler", path, method, semantics, {operation["handler"].at("id")});
        }
        for (const auto* stage : {"request", "response"}) if (operation.contains(stage) && operation[stage].contains("hooks")) {
            const auto& hooks = operation[stage]["hooks"];
            std::vector<std::string> ordered;
            for (const auto& hook : hooks) ordered.push_back(hook.at("id"));
            for (std::size_t i = 0; i < hooks.size(); ++i) append(hooks[i], SlotOwnerKind::Operation, name, stage,
                path + "/" + stage + "/hooks/" + std::to_string(i), path, method, semantics, ordered);
        }
    }
    return result;
}

CompositionDiagnostics audit_export_contracts(const std::vector<BuiltinExportContract>& contracts,
    std::shared_ptr<const DefinitionSourceSnapshot> sources, const std::vector<HookAbiDescriptor>& abis,
    std::string_view expected_registry_digest, const DefinitionLimits& limits) {
    CompositionDiagnostics result;
    const auto error = [&](const std::string& id, const std::string& code, const std::string& pointer) {
        if (result.entries.size() < limits.diagnostics) result.entries.push_back({code, id, pointer, "Compiled export audit failed", {}, {}, {}});
        else result.truncated = true;
    };
    try {
        std::set<std::string> abi_ids;
        static const std::set<std::string> stages = {"artifacts", "request", "response", "launch", "operation", "start_failure", "sink_operation",
            "install", "availability", "model_discovery", "model_download", "remote_context", "backend", "install_context", "runtime_options", "environment", "system"};
        for (const auto& abi : abis) {
            if (abi.id.empty() || !abi_ids.insert(abi.id).second || !stages.count(abi.stage) || !abi.revision || !abi.implementation_revision)
                error("", "invalid_hook_abi", "");
            std::set<std::string> operations;
            for (const auto& operation : abi.operations) if (operation.empty() || !operations.insert(operation).second) error("", "invalid_hook_abi", "");
        }
        if (!has_digest(expected_registry_digest) || export_registry_digest(contracts, abis) != expected_registry_digest)
            error("", "registry_digest_mismatch", "");
        if (contracts.size() > limits.snapshot_documents) throw LimitExceeded();
        DefinitionResolver resolver(sources, limits);
        std::set<std::pair<std::string, std::uint32_t>> keys;
        std::size_t retained = sources ? sources->logical_bytes : 0;
        Budget budget{limits, {}, retained};
        for (const auto& contract : contracts) {
            const auto& id = contract.key.id;
            if (id.empty() || !contract.key.contract || !keys.emplace(id, contract.key.contract).second || !contract.policy_revision) {
                error(id, "invalid_export_key", ""); continue;
            }
            if (!has_digest(contract.policy_digest) || export_policy_digest(contract) != contract.policy_digest) error(id, "policy_digest_mismatch", "");
            const auto resolved = resolver.resolve({DefinitionNamespace::Builtin, id, contract.key.contract});
            if (!resolved.definition) { error(id, "invalid_export_source", ""); continue; }
            const auto& definition = *resolved.definition;
            for (const auto& dependency : definition.dependencies) if (dependency.document.space != DefinitionNamespace::Builtin)
                error(id, "external_export_dependency", "/extends");
            if (definition.source_digest != contract.expected_source_digest) error(id, "source_digest_mismatch", "");
            if (definition.definition_hash != contract.expected_definition_hash) error(id, "definition_digest_mismatch", "");
            if (definition.dependency_digest != contract.expected_dependency_digest) error(id, "dependency_digest_mismatch", "");
            const auto selections = hook_selections(definition.declaration, budget);
            const auto document = std::make_shared<const DefinitionKey>(definition.key);
            const auto root = authored(definition.declaration, std::make_shared<Origin>(Origin{document, {}, {}}), budget);
            audit_owner_graph(contract, definition.declaration, budget, error);
            verify_fragment_envelope(contract, definition.declaration, budget);
            std::set<std::string> selected, policies;
            std::map<std::string, std::vector<std::string>> groups;
            for (const auto& selection : selections) {
                if (!selected.insert(slot_key(selection.slot)).second) error(id, "duplicate_hook_slot", selection.pointer);
                auto group = slot_json(selection.slot);
                group.erase("hook_id");
                const auto added = groups.emplace(group.dump(), selection.ordered_hooks);
                if (!added.second && added.first->second != selection.ordered_hooks) error(id, "ambiguous_hook_slot", selection.pointer);
            }
            for (const auto& policy : contract.slots) {
                const auto key = slot_key(policy.slot);
                if (!policies.insert(key).second) error(id, "duplicate_hook_slot", "");
                const auto selection = std::find_if(selections.begin(), selections.end(), [&](const auto& item) { return slot_key(item.slot) == key; });
                if (selection == selections.end()) { error(id, "missing_export_slot", ""); continue; }
                const auto abi = std::find_if(abis.begin(), abis.end(), [&](const auto& item) { return item.id == policy.slot.hook_id; });
                if (abi == abis.end()) error(id, "missing_hook_abi", selection->pointer);
                else if (abi->stage != policy.slot.stage || abi->revision != policy.hook_abi_revision ||
                    (!abi->operations.empty() && std::find(abi->operations.begin(), abi->operations.end(), selection->operation) == abi->operations.end()))
                    error(id, "hook_abi_mismatch", selection->pointer);
                if (parameter_digest(selection->parameters) != policy.parameter_digest) error(id, "parameter_digest_mismatch", selection->pointer);
                if (owner_semantics_digest(selection->owner_semantics) != policy.owner_semantics_digest) error(id, "owner_semantics_mismatch", selection->owner_pointer);
                if (policy.ordered_hooks != selection->ordered_hooks || std::set<std::string>(policy.ordered_hooks.begin(), policy.ordered_hooks.end()).size() != policy.ordered_hooks.size())
                    error(id, "hook_order_mismatch", selection->pointer);
                if (mutation_policy_digest(policy) != policy.mutation_policy_digest) error(id, "mutation_policy_digest_mismatch", selection->pointer);
                if ((policy.removal != SlotRemoval::Forbidden && policy.removal != SlotRemoval::WithOwner && policy.removal != SlotRemoval::Independent) ||
                    (policy.removal == SlotRemoval::WithOwner && policy.slot.owner_kind == SlotOwnerKind::Root)) error(id, "invalid_removal_policy", selection->pointer);
                std::set<std::string> targets;
                for (const auto& target : policy.sealed_inputs) {
                    if (!valid_target(target, limits) || !targets.insert(target_json(target).dump()).second) error(id, "invalid_data_policy", selection->pointer);
                    else if (!policy_target(root, target, limits)) error(id, "missing_policy_input", selection->pointer);
                }
                for (const auto& rule : policy.mutable_inputs) {
                    if (!valid_mutable_rule(rule, limits) || !targets.insert(target_json(rule.target).dump()).second) error(id, "invalid_data_policy", selection->pointer);
                    const auto path = rule.target.path + rule.target.at;
                    if (path == "/hooks" || path.rfind("/hooks/", 0) == 0 || path.find("/handler") != std::string::npos || path.find("/hook") != std::string::npos ||
                        (rule.target.path == "/operations" && (!rule.target.match || rule.target.at.empty())) ||
                        (rule.target.path == "/artifacts" && rule.target.at.empty())) error(id, "invalid_data_policy", selection->pointer);
                    if (changes_owner_semantics(rule, selections, contract.owners, limits)) error(id, "mutable_owner_semantics", selection->pointer);
                    const auto node = policy_target(root, rule.target, limits);
                    std::optional<Json> value;
                    if (node) value = materialize(node, budget);
                    if (!accepts_mutable_value(rule, value ? &*value : nullptr, budget)) error(id, "invalid_mutable_input", selection->pointer);
                }
                for (std::size_t i = 0; i < policy.mutable_inputs.size(); ++i) {
                    const auto& target = policy.mutable_inputs[i].target;
                    for (const auto& sealed : policy.sealed_inputs) if (targets_overlap(target, sealed, limits)) error(id, "ambiguous_data_policy", selection->pointer);
                    for (std::size_t j = 0; j < i; ++j) if (targets_overlap(target, policy.mutable_inputs[j].target, limits)) error(id, "ambiguous_data_policy", selection->pointer);
                }
                if (policy.effects.empty()) error(id, "invalid_effect_policy", selection->pointer);
                std::set<HookEffect> effects;
                for (const auto effect : policy.effects) if ((effect != HookEffect::Pure && effect != HookEffect::ManagedInstallRead &&
                    effect != HookEffect::ManagedInstallWrite && effect != HookEffect::ProcessProbe) || !effects.insert(effect).second) error(id, "invalid_effect_policy", selection->pointer);
                if (effects.count(HookEffect::Pure) && effects.size() != 1) error(id, "invalid_effect_policy", selection->pointer);
                static const std::set<std::string> phases = {"prepare", "backend", "install_context", "runtime_options", "environment", "launch"};
                std::set<std::string> required_phases;
                for (const auto& phase : policy.required_phases) if (!phases.count(phase) || !required_phases.insert(phase).second ||
                    !definition.declaration.value("hooks", Json::object()).contains(phase)) error(id, "invalid_phase_dependency", selection->pointer);
            }
            if (selected != policies) error(id, "uncovered_hook_slot", "");
            for (const auto& policy : contract.slots) {
                std::set<std::string> dependencies;
                for (const auto& dependency : policy.required_slots) {
                    const auto required = std::find_if(contract.slots.begin(), contract.slots.end(), [&](const auto& item) { return slot_key(item.slot) == slot_key(dependency); });
                    if (required == contract.slots.end() || slot_key(dependency) == slot_key(policy.slot) || !dependencies.insert(slot_key(dependency)).second)
                        error(id, "invalid_slot_dependency", "");
                }
            }
            std::set<std::string> active, complete;
            const auto visit = [&](const auto& self, const ExportSlotPolicy& policy) -> bool {
                const auto key = slot_key(policy.slot);
                if (complete.count(key)) return true;
                if (!active.insert(key).second) return false;
                for (const auto& dependency : policy.required_slots) {
                    const auto required = std::find_if(contract.slots.begin(), contract.slots.end(), [&](const auto& item) { return slot_key(item.slot) == slot_key(dependency); });
                    if (required != contract.slots.end() && !self(self, *required)) return false;
                }
                active.erase(key);
                complete.insert(key);
                return true;
            };
            for (const auto& policy : contract.slots) if (!visit(visit, policy)) { error(id, "slot_dependency_cycle", ""); break; }
        }
    } catch (const AuthorizationFailure& failure) { error("", failure.code, failure.source.pointer); }
    catch (const LimitExceeded&) { error("", "limit_exceeded", ""); }
    catch (const EditFailure&) { error("", "invalid_data_policy", ""); }
    catch (const std::exception&) { error("", "invalid_export_policy", ""); }
    sort_diagnostics(result);
    return result;
}

}

namespace lemon::backends {

struct BuiltinExportRegistry::Impl {
    std::vector<BuiltinExportContract> contracts;
    std::string digest;
};

const BuiltinExportContract* BuiltinExportRegistry::find(std::string_view id, std::uint32_t contract) const noexcept {
    for (const auto& item : impl_->contracts) if (item.key.id == id && item.key.contract == contract) return &item;
    return nullptr;
}
const std::string& BuiltinExportRegistry::digest() const noexcept { return impl_->digest; }
const std::vector<BuiltinExportContract>& BuiltinExportRegistry::exports() const noexcept { return impl_->contracts; }

const BuiltinExportRegistry& BuiltinCatalogLoader::installed_exports() {
    static const BuiltinExportRegistry registry = [] {
        BuiltinExportRegistry value;
        auto impl = std::make_shared<BuiltinExportRegistry::Impl>();
        impl->contracts = {llamacpp_external_contract()};
        impl->digest = "sha256:d644387f2d2838a76ace64304318e673244e8cd117e496ae4394dc0792e2b9fa";
        const auto sources = make_v2_source_snapshot(embedded_v2_sources());
        const auto audit = v2::audit_export_contracts(impl->contracts, sources.snapshot, installed_hook_abis(), impl->digest);
        if (!audit.entries.empty() || audit.truncated) throw std::logic_error("Installed export registry audit failed");
        value.impl_ = std::move(impl);
        return value;
    }();
    return registry;
}

const BuiltinExportRegistry& installed_builtin_exports() { return BuiltinCatalogLoader::installed_exports(); }

#ifdef LEMON_BACKEND_EXPORT_TESTING
std::shared_ptr<const BuiltinExportRegistry> BuiltinCatalogLoader::test_exports(
    const std::vector<BuiltinExportContract>& contracts, std::shared_ptr<const DefinitionSourceSnapshot> sources,
    const std::vector<HookAbiDescriptor>& abis, std::string_view expected_digest) {
    const auto audit = v2::audit_export_contracts(contracts, std::move(sources), abis, expected_digest);
    if (!audit.entries.empty() || audit.truncated) throw std::logic_error("Test export audit failed");
    auto value = std::shared_ptr<BuiltinExportRegistry>(new BuiltinExportRegistry());
    auto impl = std::make_shared<BuiltinExportRegistry::Impl>();
    impl->contracts = contracts;
    impl->digest = expected_digest;
    value->impl_ = std::move(impl);
    return value;
}
#endif

}
