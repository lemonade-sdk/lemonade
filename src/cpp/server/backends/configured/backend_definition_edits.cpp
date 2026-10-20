#include "backend_definition_v2_internal.h"

#include <algorithm>
#include <set>

namespace lemon::backends::v2 {
namespace {

struct Step {
    std::string key;
    std::size_t index = 0;
    bool array = false;
};
using Path = std::vector<Step>;

struct ContainerMetrics {
    Metrics value{2, 1, 1};
    bool object;
    std::size_t count = 0;
    void append(const Metrics& child, std::string_view key = {}) {
        value.bytes = checked_sum(value.bytes, checked_sum(child.bytes, count++ ? 1 : 0));
        if (object) value.bytes = checked_sum(value.bytes, checked_sum(string_bytes(key), 1));
        value.nodes = checked_sum(value.nodes, child.nodes);
        value.depth = std::max(value.depth, checked_sum(child.depth, 1));
    }
};

NodePtr child_at(const NodePtr& node, const Step& step) {
    if (step.array) return node->array.at(step.index);
    const auto it = node->object.find(step.key);
    return it == node->object.end() ? NodePtr{} : it->second;
}

std::size_t selected(const NodePtr& node, const RecordKey& key, std::string_view path) {
    const auto field = record_field(path);
    if (!field || *field != key.field || node->kind != Node::Kind::Array) throw EditFailure{"invalid_selector", "match", {std::string(path), key, ""}};
    const auto name = key.field == RecordKey::Field::Id ? "id" : "name";
    std::size_t found = node->array.size();
    for (std::size_t i = 0; i < node->array.size(); ++i) {
        const auto& record = node->array[i];
        if (record->kind != Node::Kind::Object) continue;
        const auto it = record->object.find(name);
        if (it != record->object.end() && it->second->scalar == key.value) {
            if (found != node->array.size()) throw EditFailure{"duplicate_identity", "match", {std::string(path), key, ""}};
            found = i;
        }
    }
    if (found == node->array.size()) throw EditFailure{"missing_target", "path", {std::string(path), key, ""}};
    return found;
}

Path locate(const NodePtr& root, const DefinitionTarget& target, const DefinitionLimits& limits, bool absent = false) {
    Path steps;
    auto node = root;
    const auto descend = [&](const std::vector<std::string>& tokens, bool allow_absent) {
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (node->kind == Node::Kind::Array) throw EditFailure{"array_index_forbidden", "path", target};
            if (node->kind != Node::Kind::Object) throw EditFailure{"missing_target", "path", target};
            steps.push_back({tokens[i], 0, false});
            const auto it = node->object.find(tokens[i]);
            if (it == node->object.end()) {
                if (allow_absent && i + 1 == tokens.size()) { node.reset(); return; }
                throw EditFailure{"missing_target", "path", target};
            }
            node = it->second;
        }
    };
    descend(pointer_tokens(target.path, limits), absent && !target.match);
    if (target.match) {
        steps.push_back({"", selected(node, *target.match, target.path), true});
        node = node->array[steps.back().index];
        descend(pointer_tokens(target.at, limits), absent);
    }
    return steps;
}

Metrics projected(const NodePtr& node, const Path& path, std::size_t level, const std::optional<Metrics>& replacement, bool insert, Budget& budget) {
    budget.work(0, 1);
    if (level == path.size()) return *replacement;
    const auto& step = path[level];
    ContainerMetrics metrics{{2,1,1}, node->kind == Node::Kind::Object};
    const bool final = level + 1 == path.size();
    const auto append = [&](const NodePtr& child, bool match, const std::string& key) {
        if (match && final) {
            if (replacement) { metrics.append(*replacement, key); }
            if (!insert) return;
        } else if (match) {
            metrics.append(projected(child, path, level + 1, replacement, insert, budget), key);
            return;
        }
        metrics.append(child->metrics, key);
    };
    if (node->kind == Node::Kind::Object) {
        for (const auto& [key, child] : node->object) append(child, key == step.key, key);
        if (!node->object.count(step.key)) { metrics.append(*replacement, step.key); }
    } else {
        for (std::size_t i = 0; i < node->array.size(); ++i) append(node->array[i], i == step.index, "");
        if (step.index == node->array.size()) { metrics.append(*replacement); }
    }
    budget.work(metrics.count, metrics.count);
    return metrics.value;
}

std::shared_ptr<Node> copy_node(const NodePtr& node, Budget& budget) {
    std::size_t bytes = sizeof(Node);
    for (const auto& [key, child] : node->object) bytes = checked_sum(bytes, checked_sum(key.size(), sizeof(NodePtr)));
    bytes = checked_sum(bytes, checked_product(node->array.size(), sizeof(NodePtr)));
    budget.keep(bytes);
    budget.work(bytes, 1);
    ++admission_counters.clones;
    return std::make_shared<Node>(*node);
}

void refresh(const std::shared_ptr<Node>& node) {
    ContainerMetrics metrics{{2,1,1}, node->kind == Node::Kind::Object};
    for (const auto& [key, child] : node->object) metrics.append(child->metrics, key);
    for (const auto& child : node->array) metrics.append(child->metrics);
    node->metrics = metrics.value;
}

NodePtr changed(const NodePtr& node, const Path& path, std::size_t level, const NodePtr& replacement, bool insert, Budget& budget) {
    if (level == path.size()) return replacement;
    auto result = copy_node(node, budget);
    const auto& step = path[level];
    const auto child = level + 1 == path.size() ? replacement : changed(child_at(node, step), path, level + 1, replacement, insert, budget);
    if (step.array) {
        if (insert && level + 1 == path.size()) {
            budget.keep(sizeof(NodePtr));
            result->array.insert(result->array.begin() + step.index, child);
        }
        else if (child) result->array[step.index] = child;
        else result->array.erase(result->array.begin() + step.index);
    } else {
        if (child) {
            if (!result->object.count(step.key)) budget.keep(checked_sum(step.key.size(), sizeof(NodePtr)));
            result->object[step.key] = child;
        }
        else result->object.erase(step.key);
    }
    refresh(result);
    return result;
}

Metrics merged_metrics(const NodePtr& old, const Json& overlay, Budget& budget) {
    if (!old || old->kind != Node::Kind::Object || !overlay.is_object()) return measure(overlay, budget);
    budget.work(0, 1);
    ContainerMetrics metrics{{2,1,1}, true};
    for (const auto& [key, child] : old->object) metrics.append(overlay.contains(key) ? merged_metrics(child, overlay[key], budget) : child->metrics, key);
    for (const auto& [key, child] : overlay.items()) if (!old->object.count(key)) metrics.append(measure(child, budget), key);
    return metrics.value;
}

NodePtr merged(const NodePtr& old, const Json& overlay, const OriginPtr& origin, Budget& budget) {
    if (!old || old->kind != Node::Kind::Object || !overlay.is_object()) return authored(overlay, origin, budget);
    auto result = copy_node(old, budget);
    for (const auto& [key, value] : overlay.items()) {
        const auto it = old->object.find(key);
        const auto child = merged(it == old->object.end() ? NodePtr{} : it->second, value, origin_child(origin, key, budget), budget);
        if (it == old->object.end()) budget.keep(checked_sum(key.size(), sizeof(NodePtr)));
        result->object[key] = child;
    }
    refresh(result);
    return result;
}

DefinitionEditKind edit_kind(const std::string& kind) {
    if (kind == "merge") return DefinitionEditKind::Merge;
    if (kind == "replace") return DefinitionEditKind::Replace;
    if (kind == "clear") return DefinitionEditKind::Clear;
    if (kind == "remove") return DefinitionEditKind::Remove;
    return DefinitionEditKind::Add;
}

void journal(LayerState& state, DefinitionEditKind kind, const OriginPtr& origin, const DefinitionTarget& target, Budget& budget) {
    const auto source = location(origin);
    budget.keep(checked_sum(sizeof(AuthorshipEvent), checked_sum(checked_sum(source.document.id.size(), source.pointer.size()), checked_sum(target.path.size(), checked_sum(target.at.size(), target.match ? target.match->value.size() : 0)))));
    state.journal.push_back({kind, source, target, kind != DefinitionEditKind::Merge});
}

void remember_removal(LayerState& state, const DefinitionTarget& target, const OriginPtr& origin, Budget& budget) {
    budget.keep(checked_sum(sizeof(Removal), checked_sum(target.path.size(), checked_sum(target.at.size(), target.match ? target.match->value.size() : 0))));
    state.removals.push_back({target, origin});
}

void merge_removals(LayerState& state, const NodePtr& old, const Json& overlay, const DefinitionTarget& target, const OriginPtr& origin, Budget& budget) {
    if (!old) return;
    budget.work(0, 1);
    if (old->kind == Node::Kind::Object && overlay.is_object()) {
        for (const auto& [key, child] : overlay.items()) {
            const auto it = old->object.find(key);
            if (it == old->object.end()) continue;
            auto nested = target;
            if (target.match) nested.at = join_pointer(target.at, key, budget.limits);
            else nested.path = join_pointer(target.path, key, budget.limits);
            merge_removals(state, it->second, child, nested, origin_child(origin, key, budget), budget);
        }
    } else if (old->kind != Node::Kind::Scalar) remember_removal(state, target, origin, budget);
}

}

std::optional<RecordKey::Field> record_field(std::string_view path) {
    for (const auto* registered : {"/metadata/options", "/metadata/support", "/install/cases", "/launch/argv", "/launch/custom_args/defaults", "/operations"}) if (path == registered) return RecordKey::Field::Id;
    if (path == "/launch/ports" || path == "/launch/environment/set") return RecordKey::Field::Name;
    return {};
}

DefinitionTarget target_from(const Json& value, std::string_view member) {
    DefinitionTarget target{value.at(std::string(member)).get<std::string>(), {}, value.value("at", "")};
    if (value.contains("match")) {
        const auto& match = value["match"];
        target.match = RecordKey{match.contains("id") ? RecordKey::Field::Id : RecordKey::Field::Name, match.begin().value().get<std::string>()};
    }
    return target;
}

NodePtr target_node(const NodePtr& root, const DefinitionTarget& target, const DefinitionLimits& limits) {
    auto node = root;
    for (const auto& step : locate(root, target, limits)) node = child_at(node, step);
    return node;
}

void apply_layer(LayerState& state, const Json& envelope, const DefinitionKey& key, Budget& budget) {
    budget.keep(checked_sum(sizeof(DefinitionKey), key.id.size()));
    auto document = std::make_shared<const DefinitionKey>(key);
    budget.keep(sizeof(Origin));
    auto root_origin = std::make_shared<Origin>(Origin{document, {}, {}});
    if (!state.root) state.root = authored(Json::object(), root_origin, budget);
    if (envelope.contains("merge")) {
        const auto origin = origin_child(root_origin, "merge", budget);
        budget.admit(merged_metrics(state.root, envelope["merge"], budget));
        merge_removals(state, state.root, envelope["merge"], {"", {}, ""}, origin, budget);
        state.root = merged(state.root, envelope["merge"], origin, budget);
        journal(state, DefinitionEditKind::Merge, origin, {"", {}, ""}, budget);
    }
    if (!envelope.contains("edits")) return;
    for (std::size_t i = 0; i < envelope["edits"].size(); ++i) {
        const auto& edit = envelope["edits"][i];
        const auto pointer = "/edits/" + std::to_string(i);
        const auto target = target_from(edit);
        const auto kind = edit_kind(edit["op"]);
        const auto fail = [&](const std::string& code, const std::string& member) { throw EditFailure{code, pointer + "/" + member, target}; };
        try {
            const auto tokens = pointer_tokens(target.path, budget.limits);
            static const std::set<std::string> allowed = {"artifacts", "hooks", "install", "kind", "launch", "metadata", "operations", "rationale", "unsupported_messages"};
            if (tokens.empty() || !allowed.count(tokens.front())) fail("identity_protected", "path");
            auto path = locate(state.root, target, budget.limits, kind == DefinitionEditKind::Add);
            auto old = state.root;
            for (const auto& step : path) { old = child_at(old, step); if (!old) break; }
            bool insert = false;
            if (kind == DefinitionEditKind::Add && old && old->kind == Node::Kind::Array && !target.match) {
                const auto field = record_field(target.path);
                if (!field) fail("invalid_selector", "path");
                const auto name = *field == RecordKey::Field::Id ? "id" : "name";
                const auto& value = edit["value"];
                if (!value.is_object() || !value.contains(name) || !value[name].is_string()) fail("invalid_record", "value");
                for (const auto& record : old->array) if (record->object.count(name) && record->object.at(name)->scalar == value[name]) fail("duplicate_identity", "value/" + std::string(name));
                std::size_t index = old->array.size();
                for (const auto* anchor : {"before", "after"}) if (edit.contains(anchor)) {
                    auto match = target_from(Json{{"path", target.path}, {"match", edit[anchor]}}).match;
                    try { index = selected(old, *match, target.path) + (std::string(anchor) == "after" ? 1 : 0); }
                    catch (const EditFailure& error) { fail(error.code == "missing_target" ? "missing_anchor" : error.code, anchor); }
                }
                path.push_back({"", index, true});
                old.reset();
                insert = true;
            } else {
                if (edit.contains("before") || edit.contains("after")) fail("invalid_selector", "path");
                if (kind == DefinitionEditKind::Add && old) fail("target_exists", "path");
                if (kind != DefinitionEditKind::Add && !old) fail("missing_target", "path");
            }
            if (target.match) {
                const auto name = target.match->field == RecordKey::Field::Id ? "id" : "name";
                const auto at = pointer_tokens(target.at, budget.limits);
                if (!at.empty() && at.front() == name) fail("identity_protected", "at");
                if (at.empty() && kind != DefinitionEditKind::Remove) {
                    if (kind == DefinitionEditKind::Clear || !edit["value"].is_object() ||
                        (kind == DefinitionEditKind::Replace && !edit["value"].contains(name)) ||
                        (edit["value"].contains(name) && edit["value"][name] != target.match->value)) fail("identity_protected", "value");
                }
            }
            if (kind == DefinitionEditKind::Merge && old->kind != Node::Kind::Object) fail("invalid_target_type", "path");
            if (kind == DefinitionEditKind::Clear && old->kind == Node::Kind::Scalar) fail("invalid_target_type", "path");
            std::optional<Metrics> replacement;
            Json empty;
            if (kind == DefinitionEditKind::Clear) empty = old->kind == Node::Kind::Array ? Json::array() : Json::object();
            if (kind == DefinitionEditKind::Merge) replacement = merged_metrics(old, edit["value"], budget);
            else if (kind != DefinitionEditKind::Remove) replacement = measure(kind == DefinitionEditKind::Clear ? empty : edit["value"], budget);
            budget.admit(projected(state.root, path, 0, replacement, insert, budget));
            const auto origin = origin_child(origin_child(root_origin, "edits", budget), std::to_string(i), budget);
            NodePtr node;
            if (kind == DefinitionEditKind::Merge) {
                const auto value_origin = origin_child(origin, "value", budget);
                merge_removals(state, old, edit["value"], target, value_origin, budget);
                node = merged(old, edit["value"], value_origin, budget);
            }
            else if (replacement) node = authored(kind == DefinitionEditKind::Clear ? empty : edit["value"], kind == DefinitionEditKind::Clear ? origin : origin_child(origin, "value", budget), budget);
            state.root = changed(state.root, path, 0, node, insert, budget);
            if (kind == DefinitionEditKind::Remove || kind == DefinitionEditKind::Clear || kind == DefinitionEditKind::Replace) remember_removal(state, target, origin, budget);
            journal(state, kind, origin, target, budget);
        } catch (EditFailure& error) {
            if (error.pointer.rfind("/edits/", 0) != 0) error.pointer = pointer + "/" + (error.pointer.empty() ? "path" : error.pointer);
            error.target = target;
            throw;
        }
    }
}

}

namespace lemon::backends::v2 {

void apply_identity(LayerState& state, const Json& envelope, const DefinitionKey& key, Budget& budget) {
    const Json identity{{"schema_version", 2}, {"id", envelope["id"]}, {"recipe", envelope["recipe"]}};
    budget.admit(merged_metrics(state.root, identity, budget));
    budget.keep(checked_sum(sizeof(Origin), checked_sum(sizeof(DefinitionKey), key.id.size())));
    const auto origin = std::make_shared<Origin>(Origin{std::make_shared<const DefinitionKey>(key), {}, {}});
    state.root = merged(state.root, identity, origin, budget);
}

}
