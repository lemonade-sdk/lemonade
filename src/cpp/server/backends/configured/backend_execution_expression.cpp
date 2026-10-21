#include "backend_definition_execution_internal.h"
#include "backend_definition_policy_internal.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace lemon::backends::execution {

bool type_matches(const Json& value, const std::string& type) {
    return type.empty() || (type == "string" && value.is_string()) ||
        (type == "integer" && value.is_number_integer()) || (type == "number" && value.is_number()) ||
        (type == "boolean" && value.is_boolean());
}

std::pair<std::size_t, std::size_t> model_fact(const std::string& reference, const Program& program) {
    static const std::vector<std::string> fields = {"hf_load", "embedding", "reranking", "use_mmproj",
        "mmproj_on_cpu", "use_draft_checkpoint", "context_size"};
    require(reference.rfind("/launch/hook/", 0) == 0, "unsupported_execution_feature", "");
    const auto found = std::find(fields.begin(), fields.end(), reference.substr(13));
    require(found != fields.end(), "unsupported_execution_feature", "");
    const auto producer = std::find_if(program.hooks.begin(), program.hooks.end(), [](const auto& hook) {
        return hook.contract == typed::Contract::Prepare && hook.slot.owner_kind == SlotOwnerKind::Root && hook.slot.owner_id == "launch";
    });
    require(producer != program.hooks.end(), "phase_dependency", "");
    return {static_cast<std::size_t>(producer - program.hooks.begin()), static_cast<std::size_t>(found - fields.begin())};
}

namespace {
Json typed(const Json& value, const std::string& type) {
    if (!type_matches(value, type)) throw std::invalid_argument("Incorrect expression value type");
    return value;
}
Json required(std::optional<Json> value) {
    if (!value) throw std::invalid_argument("Missing expression value");
    return std::move(*value);
}
void valid_pointer(const std::string& pointer, const std::string& at) {
    try { Json::json_pointer parsed(pointer); require(!pointer.empty() && pointer[0] == '/', "invalid_reference", at); }
    catch (const Json::exception&) { throw Failure{"invalid_reference", at}; }
}

void phase_reference(const std::string& reference, const std::string& at, const Program& program) {
    const auto& doc = program.normalized;
    const auto root = reference.substr(0, reference.find('/', 1));
    const auto& order = doc["launch"]["prepare_order"];
    const auto rank = [&](const std::string& phase) -> std::size_t {
        const auto found = std::find(order.begin(), order.end(), phase);
        return found == order.end() ? order.size() + 1 : static_cast<std::size_t>(found - order.begin());
    };
    const auto after = [&](const std::string& phase) { return rank(phase) + 1; };
    std::size_t use = at.rfind("/operations/", 0) == 0 ? order.size() + 2 : order.size() + 1;
    if (at.rfind("/install/", 0) == 0) use = rank("install") <= order.size() ? rank("install") : rank("program");
    if (at.rfind("/artifacts/", 0) == 0) use = rank("artifacts");
    if (at == "/launch/program/missing_error") use = after("program");
    if (at.rfind("/launch/custom_args/defaults/", 0) == 0) {
        const auto begin = std::string("/launch/custom_args/defaults/").size();
        const auto index = std::stoul(at.substr(begin, at.find('/', begin) - begin));
        if (doc["launch"]["custom_args"]["defaults"][index]["stage"] == "runtime_options") use = 0;
    }
    std::size_t available = 0;
    if (root == "/model" || root == "/platform" || root == "/options") available = 0;
    else if (root == "/request") available = order.size() + 2;
    else if (root == "/artifacts") available = after("artifacts");
    else if (root == "/install") available = rank(rank("install") <= order.size() ? "install" : "program");
    else if (root == "/launch") {
        require(reference.size() > 8, "unsupported_execution_feature", at);
        const auto second = reference.substr(8, reference.find('/', 8) - 8);
        if (second == "log_level") available = 0;
        else if (second == "backend") available = after("backend");
        else if (second == "hook") {
            model_fact(reference, program);
            require(rank("hook") < order.size(), "phase_dependency", at);
            available = after("hook");
        }
        else if (second == "ports") available = after("ports");
        else if (second == "executable" || second == "program" || second == "executable_directory") available = after("program");
        else throw Failure{"unsupported_execution_feature", at};
    } else throw Failure{"invalid_reference", at};
    require(available <= use, "phase_dependency", at);
}

Expression compile(const Json& value, const std::string& pointer, Program& program, Budget& budget, std::size_t depth) {
    require(depth <= budget.limits.expression_depth, "limit_exceeded", pointer);
    v2::charge(budget.metrics.expression_nodes, 1, budget.limits.expression_nodes);
    budget.work(pointer.size());
    budget.keep(sizeof(Expression) + pointer.size());
    Expression result;
    using K = Expression::Kind;
    if (!value.is_object() || value.contains("literal")) {
        const auto& literal = value.is_object() ? value["literal"] : value;
        budget.copy(literal);
        result.literal = literal;
        return result;
    }
    if (value.contains("ref") || value.contains("option")) {
        const bool option = value.contains("option");
        result.kind = option ? K::Option : K::Ref;
        result.type = value.value("type", "");
        if (value.contains("default")) {
            require(type_matches(value["default"], result.type), "invalid_type", pointer);
            budget.copy(value["default"]);
            result.default_value = value["default"];
        }
        const auto text = value[option ? "option" : "ref"].get<std::string>();
        budget.keep(text.size() + result.type.size());
        if (option) {
            const auto found = program.slots.find(text);
            require(found != program.slots.end(), "invalid_reference", pointer);
            result.slot = found->second;
        } else {
            valid_pointer(text, pointer);
            phase_reference(text, pointer, program);
            const bool produced = text.rfind("/launch/hook/", 0) == 0;
            if (produced) {
                const auto fact = model_fact(text, program);
                result.producer = fact.first;
                result.fact_field = fact.second;
                const auto type = fact.second == 6 ? "integer" : "boolean";
                require(result.type.empty() || result.type == type, "invalid_type", pointer);
                require(!result.default_value || type_matches(*result.default_value, type), "invalid_type", pointer);
                result.type = type;
            }
            if (!produced && (text.rfind("/platform/", 0) == 0 || text.rfind("/install/", 0) == 0 ||
                text.rfind("/artifacts/", 0) == 0 || text.rfind("/launch/", 0) == 0)) {
                budget.work(text.size(), v2::checked_sum(program.bindings.size(), program.normalized["launch"]["ports"].size()));
                budget.keep(v2::checked_product(v2::checked_sum(text.size(), 128), 4));
                const auto known = v2::typed_data_reference(text, program.normalized);
                require(known.has_value(), "invalid_reference", pointer);
                const auto type = known->type == MutableValueType::Integer ? "integer" :
                    known->type == MutableValueType::Boolean ? "boolean" : known->type == MutableValueType::Number ? "number" : "string";
                require(result.type.empty() || result.type == type, "invalid_type", pointer);
                require(!result.default_value || type_matches(*result.default_value, type), "invalid_type", pointer);
                require(pointer.find("/version_override") == std::string::npos || known->phase != ReferencePhase::InstallAsset,
                    "phase_dependency", pointer);
                result.type = type;
            }
            result.reference = text;
        }
        return result;
    }
    const auto name = value.begin().key();
    static const std::map<std::string, K> kinds = {{"all", K::All}, {"any", K::Any}, {"coalesce", K::Coalesce},
        {"concat", K::Concat}, {"eq", K::Eq}, {"gt", K::Gt}, {"not", K::Not}, {"parent_path", K::ParentPath},
        {"path_join", K::PathJoin}, {"present", K::Present}, {"string", K::String}, {"when", K::When}};
    const auto found = kinds.find(name);
    require(found != kinds.end(), "unsupported_execution_feature", pointer);
    result.kind = found->second;
    const auto& operands = *value.begin();
    const auto count = operands.is_array() ? operands.size() : 1;
    require(count <= budget.limits.expression_nodes - budget.metrics.expression_nodes, "limit_exceeded", pointer);
    budget.keep(v2::checked_product(count, sizeof(Expression)));
    result.children.reserve(count);
    if (operands.is_array()) for (std::size_t i = 0; i < operands.size(); ++i)
        result.children.push_back(compile(operands[i], pointer + "/" + name + "/" + std::to_string(i), program, budget, depth + 1));
    else result.children.push_back(compile(operands, pointer + "/" + name, program, budget, depth + 1));
    if (result.kind == K::Present) require(result.children[0].kind == K::Ref || result.children[0].kind == K::Option, "invalid_expression", pointer);
    return result;
}
}

std::optional<Json> Expression::evaluate(const InvocationFrame& frame, const std::vector<OptionBinding>& bindings) const {
    using K = Kind;
    if (kind == K::Literal) return literal;
    if (kind == K::Ref) {
        const Json::json_pointer pointer(reference);
        if (!frame.values.contains(pointer)) return default_value ? std::optional<Json>(typed(*default_value, type)) : std::nullopt;
        return typed(frame.values.at(pointer), type);
    }
    if (kind == K::Option) {
        const auto& binding = bindings.at(slot);
        const auto found = frame.options.find(binding.public_name);
        const auto& value = found == frame.options.end() ? binding.contract.at("default_value") : *found;
        return typed(value, type);
    }
    const auto evaluate = [&](std::size_t i) { return children[i].evaluate(frame, bindings); };
    if (kind == K::Present) return evaluate(0).has_value();
    if (kind == K::Coalesce) { for (const auto& child : children) if (auto value = child.evaluate(frame, bindings)) return value; return {}; }
    if (kind == K::All || kind == K::Any) {
        for (std::size_t i = 0; i < children.size(); ++i) {
            const bool value = required(evaluate(i)).get<bool>();
            if (kind == K::All && !value) return false;
            if (kind == K::Any && value) return true;
        }
        return kind == K::All;
    }
    if (kind == K::Not) return !required(evaluate(0)).get<bool>();
    if (kind == K::When) return required(evaluate(0)).get<bool>() ? evaluate(1) : std::nullopt;
    if (kind == K::Eq) return required(evaluate(0)) == required(evaluate(1));
    if (kind == K::Gt) {
        const auto left = required(evaluate(0)), right = required(evaluate(1));
        if (!left.is_number() || !right.is_number()) throw std::invalid_argument("gt requires numbers");
        return left > right;
    }
    if (kind == K::String) {
        const auto value = required(evaluate(0));
        if (value.is_string()) return value;
        if (value.is_boolean() || value.is_number()) return value.dump();
        throw std::invalid_argument("string requires scalar value");
    }
    if (kind == K::ParentPath) return std::filesystem::path(required(evaluate(0)).get<std::string>()).parent_path().string();
    if (kind == K::PathJoin) {
        std::filesystem::path path;
        for (std::size_t i = 0; i < children.size(); ++i) path /= required(evaluate(i)).get<std::string>();
        return path.string();
    }
    std::string result;
    for (std::size_t i = 0; i < children.size(); ++i) result += required(evaluate(i)).get<std::string>();
    return result;
}

void compile_expressions(Program& program, Budget& budget) {
    schema_walk(program.normalized, v2::schema_document()["$defs"]["Declaration"], "", budget,
        [&](const Json& value, const std::string& pointer, const std::string& ref) {
            if (ref == "#/$defs/Hook") {
                require(std::any_of(program.hooks.begin(), program.hooks.end(), [&](const auto& hook) {
                    return hook.pointer == pointer;
                }), "unsupported_execution_feature", pointer);
                return;
            }
            if (!program.expressions.count(pointer)) program.expressions.emplace(pointer, compile(value, pointer, program, budget, 0));
        });
}

}
