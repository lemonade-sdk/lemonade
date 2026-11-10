#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/configured/request_mapper.h"

#include <filesystem>

namespace lemon::backends {

namespace {
json typed(const json& value, const std::string& type) {
    if (type == "string") return value.get<std::string>();
    if (type == "boolean") return value.get<bool>();
    if (type == "integer") {
        if (!value.is_number_integer()) throw std::invalid_argument("Expected integer expression value");
        return value;
    }
    if (type == "number" && !value.is_number()) throw std::invalid_argument("Expected number expression value");
    return value;
}
json require(const std::optional<json>& value) {
    if (!value) throw std::invalid_argument("Missing expression value");
    return *value;
}
}

std::optional<json> Expression::evaluate(const json& context) const {
    using K = Kind;
    if (kind == K::Literal) return literal;
    if (kind == K::Ref) {
        const json::json_pointer pointer(reference);
        if (!context.contains(pointer)) return default_value ? std::optional<json>(typed(*default_value, type)) : std::nullopt;
        return typed(context.at(pointer), type);
    }
    if (kind == K::Present) return children.front().evaluate(context).has_value();
    if (kind == K::Coalesce) {
        for (const auto& child : children) if (auto value = child.evaluate(context)) return value;
        return std::nullopt;
    }
    if (kind == K::All || kind == K::Any) {
        for (const auto& child : children) {
            const bool value = require(child.evaluate(context)).get<bool>();
            if (kind == K::All && !value) return false;
            if (kind == K::Any && value) return true;
        }
        return kind == K::All;
    }
    if (kind == K::Not) return !require(children.front().evaluate(context)).get<bool>();
    if (kind == K::When) return require(children[0].evaluate(context)).get<bool>() ? children[1].evaluate(context) : std::nullopt;
    if (kind == K::ParentPath) return std::filesystem::path(require(children.front().evaluate(context)).get<std::string>()).parent_path().string();
    if (kind == K::PathJoin) {
        std::filesystem::path joined;
        for (const auto& child : children) joined /= child.evaluate(context).value_or(json()).get<std::filesystem::path>();
        return joined.string();
    }
    if (kind == K::Eq || kind == K::Gt) {
        const json left = require(children[0].evaluate(context));
        const json right = require(children[1].evaluate(context));
        if (kind == K::Eq) return left == right;
        if (!left.is_number() || !right.is_number()) throw std::invalid_argument("gt requires numbers");
        return left > right;
    }
    if (kind == K::String) {
        const json value = require(children.front().evaluate(context));
        if (value.is_string()) return value;
        if (value.is_boolean() || value.is_number()) return value.dump();
        throw std::invalid_argument("string requires scalar value");
    }
    std::string result;
    for (const auto& child : children) result += require(child.evaluate(context)).get<std::string>();
    return result;
}

std::optional<json> BackendDefinition::evaluate(const std::string& pointer, const json& context) const {
    return expressions.at(pointer).evaluate(context);
}

json BackendDefinition::required_value(const std::string& pointer, const json& context) const {
    auto value = evaluate(pointer, context);
    if (!value) throw std::invalid_argument("Missing expression value at " + pointer);
    return *value;
}

MappedRequest RequestMapper::map(const OperationProgram& operation, const MappingContext& context,
                                const json& request, std::optional<std::string_view> original_body) const {
    json values = context.values;
    values["request"] = request;
    MappedRequest result{&operation, operation.identity ? request : json::object(), std::nullopt, operation.path};
    if (operation.identity && operation.remove.empty() && operation.set.empty() && operation.request_hooks.empty() && original_body) {
        result.original_body = std::string(*original_body);
    }
    for (const auto& destination : operation.remove) {
        json::json_pointer pointer(destination);
        if (!result.body.contains(pointer)) continue;
        const auto token = pointer.back();
        pointer.pop_back();
        auto& parent = result.body.at(pointer);
        if (parent.is_object()) parent.erase(token);
        else if (parent.is_array()) parent.erase(static_cast<size_t>(std::stoull(token)));
    }
    for (const auto& [destination, expression] : operation.set) {
        auto value = expression.evaluate(values);
        if (!value && operation.omit_missing) continue;
        if (!value) throw std::invalid_argument("Missing mapping value at " + destination);
        result.body[json::json_pointer(destination)] = std::move(*value);
    }
    for (const auto& id : operation.request_hooks) result.body = installed_backend_hooks().target<RequestHook>(id, HookStage::Request)(std::move(result.body));
    return result;
}

}
