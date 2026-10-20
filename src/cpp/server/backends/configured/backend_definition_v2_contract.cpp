#include "backend_definition_v2_internal.h"
#include "backend_definition_v2_resources.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <tuple>

namespace lemon::backends {
namespace v2 {

std::string pointer_token(std::string_view token) {
    std::string result;
    for (char c : token) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return result;
}

const nlohmann::json& schema_document() {
    static const auto schema = nlohmann::json::parse(k_backend_definition_v2_schema);
    return schema;
}

CompositionDiagnostics check_schema_keywords(const nlohmann::json& schema) {
    static const std::set<std::string> supported = {
        "$defs", "$id", "$ref", "$schema", "additionalProperties", "anyOf", "const",
        "enum", "items", "maxItems", "maxLength", "maximum", "minItems", "minLength",
        "minimum", "oneOf", "pattern", "properties", "required", "type", "uniqueItems"
    };
    CompositionDiagnostics result;
    const auto visit = [&](const auto& self, const nlohmann::json& rule, const std::string& path) -> void {
        if (!rule.is_object()) return;
        for (const auto& [key, value] : rule.items()) {
            const auto pointer = path + "/" + pointer_token(key);
            if (!supported.count(key)) {
                if (result.entries.size() < 100) result.entries.push_back({"unsupported_schema_keyword", "", pointer, "Unsupported schema keyword", {}, {}, {}});
                else result.truncated = true;
            } else if (key == "properties" || key == "$defs") {
                for (const auto& [name, child] : value.items()) self(self, child, pointer + "/" + pointer_token(name));
            } else if (key == "anyOf" || key == "oneOf") {
                for (std::size_t i = 0; i < value.size(); ++i) self(self, value[i], pointer + "/" + std::to_string(i));
            } else if (key == "items" || key == "additionalProperties") self(self, value, pointer);
            else if (key == "$ref" && (!value.is_string() || value.get_ref<const std::string&>().rfind("#/$defs/", 0) != 0 || !schema.contains(nlohmann::json::json_pointer(value.get<std::string>().substr(1))))) {
                result.entries.push_back({"invalid_schema_reference", "", pointer, "Schema references must resolve locally", {}, {}, {}});
            }
        }
    };
    visit(visit, schema, "");
    return result;
}

}
namespace {
using Json = nlohmann::json;

bool matches_type(const Json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    return type == "null" && value.is_null();
}

class StructuralChecker {
public:
    explicit StructuralChecker(const DefinitionLimits& limits) : limits_(limits) {}

    CompositionDiagnostics result;

    void error(const std::string& code, const std::string& path, const std::string& message) {
        ++errors_;
        if (result.entries.size() < limits_.diagnostics) result.entries.push_back({code, "", path, message, {}, {}, {}});
        else result.truncated = true;
    }

    void check(const Json& value, const Json& rule, const std::string& path, std::size_t depth = 0, std::size_t refs = 0) {
        if (exhausted_) return;
        if (visits_ >= limits_.work_nodes || depth > limits_.effective_depth || refs > 128 || path.size() > limits_.pointer_bytes) {
            exhausted_ = true;
            error("limit_exceeded", "", "Structural validation budget exceeded");
            return;
        }
        ++visits_;
        if (rule.contains("$ref")) {
            check(value, v2::schema_document().at(Json::json_pointer(rule["$ref"].get<std::string>().substr(1))), path, depth, refs + 1);
        }
        for (const auto* keyword : {"anyOf", "oneOf"}) {
            if (!rule.contains(keyword)) continue;
            std::size_t matches = 0;
            for (const auto& alternative : rule[keyword]) {
                auto saved = std::move(result);
                result = {};
                const auto previous_errors = errors_;
                check(value, alternative, path, depth, refs + 1);
                if (errors_ == previous_errors) ++matches;
                errors_ = previous_errors;
                result = std::move(saved);
                if (exhausted_) { error("limit_exceeded", "", "Structural validation budget exceeded"); return; }
            }
            if (matches == 0 || (std::string(keyword) == "oneOf" && matches != 1)) error("invalid_shape", path, "No unique valid structural alternative");
        }
        if (rule.contains("type")) {
            const auto& types = rule["type"];
            bool valid = types.is_string() && matches_type(value, types.get<std::string>());
            if (types.is_array()) for (const auto& type : types) valid |= matches_type(value, type.get<std::string>());
            if (!valid) { error("invalid_type", path, "Incorrect value type"); return; }
        }
        if (rule.contains("const") && value != rule["const"]) error("invalid_value", path, "Incorrect constant");
        if (rule.contains("enum") && std::find(rule["enum"].begin(), rule["enum"].end(), value) == rule["enum"].end()) error("invalid_value", path, "Unknown enum value");
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            const auto length = std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
            if (rule.contains("minLength") && static_cast<std::size_t>(length) < rule["minLength"].get<std::size_t>()) error("invalid_value", path, "String too short");
            if (rule.contains("maxLength") && static_cast<std::size_t>(length) > rule["maxLength"].get<std::size_t>()) error("invalid_value", path, "String too long");
            if (rule.contains("pattern") && !std::regex_match(text, std::regex(rule["pattern"].get<std::string>()))) error("invalid_value", path, "Invalid string format");
        }
        if (value.is_number()) {
            if (value.is_number_float() && !std::isfinite(value.get<double>())) error("invalid_value", path, "Nonfinite number");
            if (rule.contains("minimum") && value < rule["minimum"]) error("invalid_value", path, "Below minimum");
            if (rule.contains("maximum") && value > rule["maximum"]) error("invalid_value", path, "Above maximum");
        }
        if (value.is_array()) {
            if (rule.contains("minItems") && value.size() < rule["minItems"].get<std::size_t>()) error("invalid_value", path, "Array too short");
            if (rule.contains("maxItems") && value.size() > rule["maxItems"].get<std::size_t>()) error("invalid_value", path, "Array too long");
            for (std::size_t i = 0; i < value.size() && !exhausted_; ++i) {
                if (rule.value("uniqueItems", false)) for (std::size_t j = 0; j < i; ++j) {
                    if (visits_ >= limits_.work_nodes) { exhausted_ = true; error("limit_exceeded", "", "Structural validation budget exceeded"); return; }
                    ++visits_;
                    if (value[i] == value[j]) error("duplicate_value", path + "/" + std::to_string(i), "Duplicate array value");
                }
                if (rule.contains("items")) descend(value[i], rule["items"], path, std::to_string(i), depth);
            }
        }
        if (value.is_object()) {
            if (rule.contains("required")) for (const auto& key : rule["required"]) if (!value.contains(key.get<std::string>())) error("required", path + "/" + v2::pointer_token(key.get<std::string>()), "Required field absent");
            for (const auto& [key, child] : value.items()) {
                if (exhausted_) break;
                if (rule.contains("properties") && rule["properties"].contains(key)) descend(child, rule["properties"][key], path, key, depth);
                else if (rule.contains("additionalProperties")) {
                    if (rule["additionalProperties"].is_object()) descend(child, rule["additionalProperties"], path, key, depth);
                    else if (rule["additionalProperties"] == false) error("unknown_key", path + "/" + v2::pointer_token(key), "Unknown field");
                }
            }
        }
    }

private:
    void descend(const Json& value, const Json& rule, const std::string& path, const std::string& token, std::size_t depth) {
        if (path.size() >= limits_.pointer_bytes || token.size() > (limits_.pointer_bytes - path.size() - 1) / 2) {
            exhausted_ = true;
            error("limit_exceeded", "", "Structural pointer budget exceeded");
            return;
        }
        check(value, rule, path + "/" + v2::pointer_token(token), depth + 1);
    }

    const DefinitionLimits& limits_;
    std::size_t errors_ = 0;
    std::size_t visits_ = 0;
    bool exhausted_ = false;
};
}

CompositionDiagnostics validate_v2_structure(const nlohmann::json& value, DefinitionV2SchemaRoot root, const DefinitionLimits& limits) {
    StructuralChecker checker(limits);
    const auto& schema = v2::schema_document();
    checker.check(value, root == DefinitionV2SchemaRoot::Envelope ? schema : schema["$defs"]["Declaration"], "");
    std::sort(checker.result.entries.begin(), checker.result.entries.end(), [](const auto& left, const auto& right) {
        return std::tie(left.definition, left.pointer, left.code) < std::tie(right.definition, right.pointer, right.code);
    });
    return std::move(checker.result);
}

}
