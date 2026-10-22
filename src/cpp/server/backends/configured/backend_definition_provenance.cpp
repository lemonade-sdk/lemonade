#include "backend_definition_v2_internal.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace lemon::backends::v2 {

std::string join_pointer(std::string_view left, std::string_view token, const DefinitionLimits& limits) {
    std::size_t length = checked_sum(left.size(), 1);
    for (char c : token) charge(length, c == '~' || c == '/' ? 2 : 1, limits.pointer_bytes);
    if (length > limits.pointer_bytes) throw LimitExceeded();
    ++admission_counters.pointers;
    return std::string(left) + "/" + pointer_token(token);
}

std::vector<std::string> pointer_tokens(std::string_view pointer, const DefinitionLimits& limits) {
    if (pointer.size() > limits.pointer_bytes) throw LimitExceeded();
    if (pointer.empty()) return {};
    if (pointer[0] != '/') throw EditFailure{"invalid_pointer", "", {std::string(pointer), {}, ""}};
    ++admission_counters.pointers;
    std::vector<std::string> result;
    for (std::size_t start = 1; start <= pointer.size();) {
        const auto end = pointer.find('/', start);
        const auto token = pointer.substr(start, end == std::string_view::npos ? pointer.size() - start : end - start);
        std::string decoded;
        decoded.reserve(token.size());
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (token[i] != '~') decoded += token[i];
            else {
                if (++i == token.size() || (token[i] != '0' && token[i] != '1')) throw EditFailure{"invalid_pointer", "", {std::string(pointer), {}, ""}};
                decoded += token[i] == '0' ? '~' : '/';
            }
        }
        result.push_back(std::move(decoded));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

OriginPtr origin_child(const OriginPtr& parent, std::string_view token, Budget& budget) {
    std::size_t length = 0;
    const auto count = [&](std::string_view segment) {
        charge(length, 1, budget.limits.pointer_bytes);
        for (char c : segment) charge(length, c == '~' || c == '/' ? 2 : 1, budget.limits.pointer_bytes);
    };
    for (auto current = parent; current && current->parent; current = current->parent) count(*current->token);
    count(token);
    auto segment = budget.segments.find(std::string(token));
    if (segment == budget.segments.end()) {
        budget.keep(checked_sum(sizeof(std::string) + sizeof(std::shared_ptr<const std::string>), checked_product(token.size(), 2)));
        const auto stored = std::make_shared<const std::string>(token);
        segment = budget.segments.emplace(*stored, stored).first;
    }
    budget.keep(sizeof(Origin));
    ++admission_counters.origins;
    return std::make_shared<Origin>(Origin{parent->document, parent, segment->second});
}

SourceLocation location(const OriginPtr& origin) {
    std::vector<std::string_view> tokens;
    for (auto current = origin; current && current->parent; current = current->parent) tokens.push_back(*current->token);
    std::string pointer;
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) pointer += "/" + pointer_token(*it);
    return {*origin->document, std::move(pointer)};
}

std::optional<SourceLocation> removed_at(const std::vector<Removal>& removals, const DefinitionTarget& target) {
    const auto same_match = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() && (!left || (left->field == right->field && left->value == right->value));
    };
    for (auto it = removals.rbegin(); it != removals.rend(); ++it) {
        const auto& old = it->target;
        if (old.path == target.path && same_match(old.match, target.match) &&
            (old.at == target.at || (!old.at.empty() && target.at.rfind(old.at + "/", 0) == 0) || old.at.empty())) return location(it->origin);
        if (!old.match && (old.path == target.path || target.path.rfind(old.path + "/", 0) == 0)) return location(it->origin);
    }
    return {};
}

void sort_diagnostics(CompositionDiagnostics& diagnostics) {
    std::sort(diagnostics.entries.begin(), diagnostics.entries.end(), [](const auto& left, const auto& right) {
        return std::tie(left.definition, left.pointer, left.code) < std::tie(right.definition, right.pointer, right.code);
    });
}

}

namespace lemon::backends {

std::optional<SourceLocation> ResolutionProvenance::origin_at(std::string_view pointer) const {
    if (!impl_) return {};
    try {
        auto node = impl_->root;
        for (const auto& token : v2::pointer_tokens(pointer, {})) {
            if (node->kind == v2::Node::Kind::Object) {
                const auto it = node->object.find(token);
                if (it == node->object.end()) return {};
                node = it->second;
            } else if (node->kind == v2::Node::Kind::Array) {
                if (token.empty() || (token.size() > 1 && token[0] == '0') || token.find_first_not_of("0123456789") != std::string::npos) return {};
                std::size_t index = 0;
                for (char c : token) {
                    if (index > node->array.size() / 10) return {};
                    index = index * 10 + static_cast<std::size_t>(c - '0');
                    if (index >= node->array.size()) return {};
                }
                node = node->array[index];
            } else return {};
        }
        return v2::location(node->origin);
    } catch (const v2::EditFailure&) { return {}; }
    catch (const v2::LimitExceeded&) { return {}; }
}

std::optional<SourceLocation> ResolutionProvenance::removal_at(const DefinitionTarget& target) const {
    return impl_ ? v2::removed_at(impl_->removals, target) : std::nullopt;
}

const std::vector<AuthorshipEvent>& ResolutionProvenance::authorship() const noexcept {
    static const std::vector<AuthorshipEvent> empty;
    return impl_ ? impl_->journal : empty;
}

}

namespace lemon::backends::v2 {

std::string config_key(const std::string& name) {
    const auto suffix = [&](const std::string& ending) { return name.size() >= ending.size() && name.compare(name.size() - ending.size(), ending.size(), ending) == 0; };
    return suffix("_backend") ? "backend" : suffix("_args") ? "args" : suffix("_device") ? "device" : name;
}

NodePtr effective_node(NodePtr node, std::string_view pointer, const DefinitionLimits& limits) {
    for (const auto& token : pointer_tokens(pointer, limits)) {
        if (node->kind == Node::Kind::Object) {
            const auto it = node->object.find(token);
            if (it == node->object.end()) break;
            node = it->second;
        } else if (node->kind == Node::Kind::Array) {
            if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) break;
            std::size_t index = 0;
            bool valid = true;
            for (char c : token) {
                if (index > node->array.size() / 10) { valid = false; break; }
                index = index * 10 + static_cast<std::size_t>(c - '0');
                if (index >= node->array.size()) { valid = false; break; }
            }
            if (!valid) break;
            node = node->array[index];
        } else break;
    }
    return node;
}

CompositionDiagnostics relationships(const Json& doc, const LayerState& state, Budget& budget) {
    CompositionDiagnostics result;
    const auto records = [&] {
        std::size_t count = result.entries.size();
        for (const auto& diagnostic : result.entries) count = checked_sum(count, diagnostic.related.size());
        return count;
    };
    const auto add_related = [&](CompositionDiagnostic& diagnostic, const SourceLocation& origin) {
        if (records() >= budget.limits.diagnostics) { result.truncated = true; return; }
        budget.keep(checked_sum(sizeof(SourceLocation), checked_sum(origin.document.id.size(), origin.pointer.size())));
        diagnostic.related.push_back(origin);
    };
    const auto error = [&](const std::string& code, const std::string& pointer, const std::optional<std::string>& related = {}) -> CompositionDiagnostic* {
        if (records() >= budget.limits.diagnostics) { result.truncated = true; return nullptr; }
        budget.keep(checked_sum(sizeof(CompositionDiagnostic), pointer.size()));
        result.entries.push_back({code, "", pointer, "Declaration relationship failed", pointer, {}, {}});
        if (related) add_related(result.entries.back(), location(effective_node(state.root, *related, budget.limits)->origin));
        return &result.entries.back();
    };
    for (const auto* path : {"/metadata/options", "/metadata/support", "/install/cases", "/launch/argv", "/launch/custom_args/defaults", "/operations", "/launch/ports", "/launch/environment/set"}) {
        const auto pointer = Json::json_pointer(path);
        if (!doc.contains(pointer)) continue;
        const auto field = *record_field(path) == RecordKey::Field::Id ? "id" : "name";
        std::map<std::string, std::string> seen;
        const auto& records = doc.at(pointer);
        for (std::size_t i = 0; i < records.size(); ++i) {
            budget.work(0, 1);
            const auto p = std::string(path) + "/" + std::to_string(i) + "/" + field;
            const auto value = records[i][field].get<std::string>();
            budget.keep(checked_sum(value.size(), p.size()));
            const auto insertion = seen.emplace(value, p);
            if (!insertion.second) error("duplicate_identity", p, insertion.first->second);
        }
    }
    std::set<std::string> ids;
    std::map<std::string, std::string> names, keys;
    if (doc["metadata"].contains("options")) for (std::size_t i = 0; i < doc["metadata"]["options"].size(); ++i) {
        const auto& option = doc["metadata"]["options"][i];
        const auto p = "/metadata/options/" + std::to_string(i);
        const auto id = option["id"].get<std::string>();
        const auto name = option["name"].get<std::string>();
        const auto key = config_key(name);
        budget.keep(checked_sum(id.size(), checked_sum(name.size(), checked_sum(key.size(), p.size() * 2))));
        ids.insert(id);
        auto insertion = names.emplace(name, p);
        if (!insertion.second) error("option_collision", p + "/name", insertion.first->second);
        insertion = keys.emplace(key, p);
        if (!insertion.second) error("config_key_collision", p + "/name", insertion.first->second);
    }
    const auto option_reference = [&](const Json& value, const std::string& pointer) {
        if (value.is_string() && !ids.count(value.get<std::string>())) {
            auto* diagnostic = error("invalid_reference", pointer, "/metadata/options");
            const auto removal = removed_at(state.removals, {"/metadata/options", RecordKey{RecordKey::Field::Id, value.get<std::string>()}, ""});
            if (removal && diagnostic) add_related(*diagnostic, *removal);
        }
    };
    const auto expression = [&](const auto& self, const Json& value, const std::string& pointer) -> void {
        budget.work(0, 1);
        if (!value.is_object() || value.contains("literal")) return;
        if (value.contains("option")) option_reference(value["option"], join_pointer(pointer, "option", budget.limits));
        for (const auto& [key, child] : value.items()) {
            if (key == "default" || key == "option" || key == "ref" || key == "type") continue;
            const auto path = join_pointer(pointer, key, budget.limits);
            if (child.is_array()) for (std::size_t i = 0; i < child.size(); ++i) self(self, child[i], join_pointer(path, std::to_string(i), budget.limits));
            else self(self, child, path);
        }
    };
    std::set<std::string> expressions;
    const auto walk = [&](const auto& self, const Json& value, const Json& rule, const std::string& pointer) -> void {
        budget.work(0, 1);
        if (rule.contains("$ref")) {
            const auto ref = rule["$ref"].get<std::string>();
            if (ref == "#/$defs/Expression") {
                if (!expressions.count(pointer)) {
                    budget.keep(pointer.size());
                    expressions.insert(pointer);
                    expression(expression, value, pointer);
                }
            } else self(self, value, schema_document().at(Json::json_pointer(ref.substr(1))), pointer);
        }
        for (const auto* alternative : {"oneOf", "anyOf"}) if (rule.contains(alternative)) for (const auto& branch : rule[alternative]) self(self, value, branch, pointer);
        if (value.is_object()) for (const auto& [key, child] : value.items()) {
            if (rule.contains("properties") && rule["properties"].contains(key)) self(self, child, rule["properties"][key], join_pointer(pointer, key, budget.limits));
            else if (rule.contains("additionalProperties") && rule["additionalProperties"].is_object() && !rule["additionalProperties"].empty()) self(self, child, rule["additionalProperties"], join_pointer(pointer, key, budget.limits));
        }
        if (value.is_array() && rule.contains("items")) for (std::size_t i = 0; i < value.size(); ++i) self(self, value[i], rule["items"], join_pointer(pointer, std::to_string(i), budget.limits));
    };
    walk(walk, doc, schema_document()["$defs"]["Declaration"], "");
    for (const auto* path : {"/launch/backend/option", "/launch/custom_args/option"}) if (doc.contains(Json::json_pointer(path))) option_reference(doc.at(Json::json_pointer(path)), path);
    const auto& launch = doc["launch"];
    const auto& order = launch["prepare_order"];
    const auto position = [&](const std::string& name) { return std::find(order.begin(), order.end(), name); };
    const auto& hooks = doc.contains("hooks") ? doc["hooks"] : Json::object();
    const bool install = position("install") != order.end();
    if (position("backend") == order.end() || position("artifacts") == order.end() || position("program") == order.end() || position("ports") == order.end() ||
        position("backend") >= position("program") ||
        (install ? position("backend") >= position("install") || position("install") >= position("program") || position("install") >= position("ports") : (doc["install"]["strategy"] != "external_only" && position("program") >= position("ports")))) error("invalid_relationship", "/launch/prepare_order");
    if (install && (launch["program"]["source"] != "managed" || doc["install"]["strategy"] != "github_release")) error("invalid_relationship", "/launch/prepare_order", "/install");
    if ((position("hook") != order.end()) != hooks.contains("launch")) error("invalid_relationship", "/launch/prepare_order", "/hooks");
    if (position("custom_args") != order.end() && !launch.contains("custom_args")) error("invalid_relationship", "/launch/prepare_order");
    if (launch["environment"]["preset"] == "hook" && !hooks.contains("environment")) error("invalid_relationship", "/launch/environment/preset", "/hooks");
    if (doc["install"]["strategy"] == "github_release" && (!doc["install"].contains("cases") || doc["install"]["cases"].empty())) error("invalid_relationship", "/install/cases");
    std::set<std::string> ports;
    for (const auto& port : launch["ports"]) ports.insert(port["name"].get<std::string>());
    if (!ports.count(launch["readiness"]["port"].get<std::string>()) || launch["watchdog"]["port"] != launch["readiness"]["port"] || launch["watchdog"]["path"] != launch["readiness"]["path"]) error("invalid_relationship", "/launch/watchdog", "/launch/readiness");
    if (doc.contains("rationale")) for (std::size_t i = 0; i < doc["rationale"].size(); ++i) {
        const auto target = target_from(doc["rationale"][i], "subject");
        try { target_node(state.root, target, budget.limits); }
        catch (const EditFailure& failure) {
            const auto pointer = "/rationale/" + std::to_string(i) + "/subject";
            auto* diagnostic = error(failure.code == "array_index_forbidden" ? failure.code : "invalid_reference", pointer);
            const auto removal = removed_at(state.removals, target);
            if (removal && diagnostic) add_related(*diagnostic, *removal);
        }
    }
    return result;
}

}
