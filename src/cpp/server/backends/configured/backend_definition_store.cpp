#include "lemon/backends/configured/backend_definition_store.h"
#include "backend_definition_resources.h"

#include <algorithm>
#include <regex>
#include <set>
#include <tuple>

namespace lemon::backends {
namespace {

std::string escape(const std::string& token) {
    std::string result;
    for (char c : token) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return result;
}
bool type_matches(const json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    return type == "null" && value.is_null();
}

class Validator {
public:
    Validator(const DefinitionSource& input, DefinitionValidationResult& output)
        : source(input), result(output) {}
    void error(const std::string& code, const std::string& pointer, const std::string& message) {
        if (result.diagnostics.size() < 100) result.diagnostics.push_back({code, id, source.file, pointer, message});
    }
    void schema(const json& value, const json& rule, const json& root, const std::string& pointer, size_t depth = 0) {
        if (depth > 128) { error("limit_exceeded", pointer, "Structural depth exceeded"); return; }
        if (rule.contains("$ref")) { schema(value, root.at(json::json_pointer(rule["$ref"].get<std::string>().substr(1))), root, pointer, depth + 1); return; }
        for (const auto* alternatives : {"anyOf", "oneOf"}) {
            if (!rule.contains(alternatives)) continue;
            size_t matches = 0;
            for (const auto& option : rule[alternatives]) {
                DefinitionValidationResult probe;
                Validator validator(source, probe);
                validator.schema(value, option, root, pointer, depth + 1);
                if (probe.diagnostics.empty()) ++matches;
            }
            if (!matches || (std::string(alternatives) == "oneOf" && matches != 1)) error("invalid_shape", pointer, "No unique valid structural alternative");
            return;
        }
        if (rule.contains("type")) {
            const auto types = rule["type"].is_array() ? rule["type"] : json::array({rule["type"]});
            bool valid = false;
            for (const auto& type : types) valid |= type_matches(value, type.get<std::string>());
            if (!valid) { error("invalid_type", pointer, "Incorrect value type"); return; }
        }
        if (rule.contains("const") && value != rule["const"]) error("invalid_value", pointer, "Incorrect constant");
        if (rule.contains("enum") && std::find(rule["enum"].begin(), rule["enum"].end(), value) == rule["enum"].end()) error("invalid_value", pointer, "Unknown enum value");
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            if (rule.contains("minLength") && text.size() < rule["minLength"].get<size_t>()) error("invalid_value", pointer, "String too short");
            if (rule.contains("pattern") && !std::regex_match(text, std::regex(rule["pattern"].get<std::string>()))) error("invalid_value", pointer, "Invalid identifier or digest");
        }
        if (value.is_number()) {
            if (rule.contains("minimum") && value < rule["minimum"]) error("invalid_value", pointer, "Below minimum");
            if (rule.contains("maximum") && value > rule["maximum"]) error("invalid_value", pointer, "Above maximum");
        }
        if (value.is_array()) {
            if (rule.contains("minItems") && value.size() < rule["minItems"].get<size_t>()) error("invalid_value", pointer, "Array too short");
            if (rule.contains("maxItems") && value.size() > rule["maxItems"].get<size_t>()) error("invalid_value", pointer, "Array too long");
            for (size_t i = 0; i < value.size(); ++i) {
                if (rule.value("uniqueItems", false)) for (size_t j = 0; j < i; ++j) if (value[i] == value[j]) error("duplicate_value", pointer + "/" + std::to_string(i), "Duplicate array value");
                if (rule.contains("items")) schema(value[i], rule["items"], root, pointer + "/" + std::to_string(i), depth + 1);
            }
        }
        if (value.is_object()) {
            for (const auto& key : rule.value("required", json::array())) if (!value.contains(key.get<std::string>())) error("required", pointer + "/" + escape(key.get<std::string>()), "Required field absent");
            const auto properties = rule.value("properties", json::object());
            for (const auto& [key, field] : value.items()) {
                const auto path = pointer + "/" + escape(key);
                if (properties.contains(key)) schema(field, properties[key], root, path, depth + 1);
                else if (rule.contains("additionalProperties")) {
                    if (rule["additionalProperties"].is_object()) schema(field, rule["additionalProperties"], root, path, depth + 1);
                    else if (rule["additionalProperties"] == false) error("unknown_key", path, "Unknown field");
                }
            }
        }
    }
    Expression compile(const json& value, const std::string& pointer, size_t depth = 0) {
        Expression expression;
        if (++nodes > 4096 || depth > 32) { error("limit_exceeded", pointer, "Expression depth/node limit exceeded"); return expression; }
        if (!value.is_object()) { expression.literal = value; return expression; }
        using K = Expression::Kind;
        if (value.contains("literal")) { expression.literal = value["literal"]; return expression; }
        if (value.contains("ref")) {
            expression.kind = K::Ref;
            expression.reference = value["ref"];
            expression.type = value.value("type", "");
            if (value.contains("default")) expression.default_value = value["default"];
            pointer_valid(expression.reference, pointer + "/ref", false);
            const auto slash = expression.reference.find('/', 1);
            const auto root = expression.reference.substr(0, slash);
            const std::set<std::string> roots = {"/options", "/model", "/artifacts", "/platform", "/install", "/launch", "/request"};
            if (!roots.count(root)) error("invalid_reference", pointer + "/ref", "Unknown expression context root");
            if (expression.default_value && !expression.type.empty() && !type_matches(*expression.default_value, expression.type)) error("invalid_type", pointer + "/default", "Reference default has wrong type");
            return expression;
        }
        const auto& entry = *value.begin();
        const auto name = value.begin().key();
        const std::map<std::string, K> kinds = {{"concat", K::Concat}, {"string", K::String}, {"coalesce", K::Coalesce}, {"present", K::Present}, {"eq", K::Eq}, {"gt", K::Gt}, {"all", K::All}, {"any", K::Any}, {"not", K::Not}};
        expression.kind = kinds.at(name);
        if (name == "string" || name == "present" || name == "not") expression.children.push_back(compile(entry, pointer + "/" + name, depth + 1));
        else for (size_t i = 0; i < entry.size(); ++i) expression.children.push_back(compile(entry[i], pointer + "/" + name + "/" + std::to_string(i), depth + 1));
        if (name == "present" && expression.children.front().kind != K::Ref) error("invalid_expression", pointer, "present requires a reference");
        return expression;
    }
    void pointer_valid(const std::string& text, const std::string& pointer, bool allow_root) {
        try {
            json::json_pointer parsed(text);
            if ((!allow_root && text.empty()) || (!text.empty() && text[0] != '/')) throw std::invalid_argument("Invalid pointer");
        } catch (const std::exception&) { error("invalid_pointer", pointer, "Invalid JSON pointer"); }
    }
    void expression(BackendDefinition& definition, const std::string& pointer) {
        definition.expressions.emplace(pointer, compile(definition.records.at(json::json_pointer(pointer)), pointer));
    }
    void relationships(BackendDefinition& definition) {
        auto& doc = definition.records;
        if (doc["kind"] != "local") error("unsupported_feature", "/kind", "Remote execution is not installed");
        if (!doc.contains("launch") || doc.contains("remote")) { error("invalid_relationship", "/launch", "Local definition requires only launch"); return; }
        auto& metadata = doc["metadata"];
        for (const auto* key : {"display_name", "server_name", "binary"}) if (!metadata.contains(key)) error("required", std::string("/metadata/") + key, "Local metadata required");
        metadata["log_category"] = metadata.value("log_category", metadata.value("server_name", ""));
        metadata["log_level_policy"] = metadata.value("log_level_policy", "passthrough");
        metadata["options"] = metadata.value("options", json::array());
        std::set<std::string> generated_keys;
        if (metadata.value("selectable_backend", false)) generated_keys.insert("backend");
        if (metadata.value("takes_args", false)) generated_keys.insert("args");
        for (const auto& variant : metadata.value("arg_variants", json::array())) generated_keys.insert(variant.get<std::string>() + "_args");
        for (const auto& variant : metadata.value("bin_variants", json::array())) generated_keys.insert(variant.get<std::string>() + "_bin");
        const auto extra = metadata.value("config_extra", json::object());
        for (const auto& [key, value] : extra.items()) if (generated_keys.count(key) && !value.is_string()) error("invalid_type", "/metadata/config_extra/" + escape(key), "Generated config field type mismatch");
        const auto declared_modes = metadata.value("supported_modes", json::array({"chat"}));
        for (const auto& mode : declared_modes) {
            ModelType type;
            if (!deployment_mode_of(mode.get<std::string>(), type)) error("invalid_value", "/metadata/supported_modes", "Unknown deployment mode");
        }
        std::set<std::string> option_names;
        for (size_t i = 0; i < metadata["options"].size(); ++i) {
            auto& option = metadata["options"][i];
            const std::string p = "/metadata/options/" + std::to_string(i);
            const std::string name = option["name"];
            if (!option_names.insert(name).second) error("option_collision", p + "/name", "Duplicate option ownership");
            const std::set<std::string> reserved = {"model", "prompt", "stream", "recipe", "checkpoints", "backend", "type"};
            if (reserved.count(name)) error("option_collision", p + "/name", "Reserved option name");
            if (source.origin == DefinitionOrigin::External && !option.value("cli_flag", "").empty()) error("invalid_value", p + "/cli_flag", "External named flags are forbidden");
            const auto sentinels = option.value("unset_values", json::array({nullptr}));
            auto valid = [&](const json& value) { return type_matches(value, option["value_type"]) || std::find(sentinels.begin(), sentinels.end(), value) != sentinels.end(); };
            if (!valid(option["default_value"])) error("invalid_type", p + "/default_value", "Option default type mismatch");
            for (const auto& value : option.value("enum", json::array())) if (!valid(value)) error("invalid_type", p + "/enum", "Option enum type mismatch");
            for (const auto& value : option.value("enum", json::array())) {
                if (std::find(sentinels.begin(), sentinels.end(), value) != sentinels.end()) continue;
                if ((option.contains("minimum") && (!value.is_number() || value < option["minimum"])) ||
                    (option.contains("maximum") && (!value.is_number() || value > option["maximum"]))) error("invalid_value", p + "/enum", "Enum value outside option bounds");
            }
            const auto default_value = option["default_value"];
            const bool sentinel = std::find(sentinels.begin(), sentinels.end(), default_value) != sentinels.end();
            if (!sentinel) {
                if (option.contains("enum") && std::find(option["enum"].begin(), option["enum"].end(), default_value) == option["enum"].end()) error("invalid_value", p + "/default_value", "Default absent from enum");
                if (option.contains("minimum") && (!default_value.is_number() || default_value < option["minimum"])) error("invalid_value", p + "/default_value", "Default below minimum");
                if (option.contains("maximum") && (!default_value.is_number() || default_value > option["maximum"])) error("invalid_value", p + "/default_value", "Default above maximum");
            }
            if (option.contains("minimum") && option.contains("maximum") && option["minimum"] > option["maximum"]) error("invalid_value", p, "Option bounds reversed");
            if (option.contains("ui") && option["ui"]["control"] == "select") {
                const bool choices = option["ui"].contains("choices");
                if (choices == option.contains("enum")) error("invalid_relationship", p + "/ui", "Select requires one choice source");
                if (choices && doc["launch"]["backend"].value("option", "") != name) error("invalid_relationship", p + "/ui/choices", "Only backend option uses supported_backends");
            }
        }
        const auto& launch = doc["launch"];
        auto position = [&](const char* name) { return std::find(launch["prepare_order"].begin(), launch["prepare_order"].end(), name); };
        if (!(position("backend") < position("program") && position("program") < position("ports"))) error("invalid_relationship", "/launch/prepare_order", "Preparation dependency order violated");
        if (launch["backend"].contains("option") && !option_names.count(launch["backend"]["option"])) error("invalid_reference", "/launch/backend/option", "Undeclared option");
        std::set<std::string> ports;
        for (size_t i = 0; i < launch["ports"].size(); ++i) if (!ports.insert(launch["ports"][i]["name"]).second) error("duplicate_value", "/launch/ports/" + std::to_string(i) + "/name", "Duplicate port");
        if (!ports.count(launch["readiness"]["port"]) || launch["watchdog"]["port"] != launch["readiness"]["port"] || launch["watchdog"]["path"] != launch["readiness"]["path"]) error("invalid_relationship", "/launch/watchdog", "Readiness/watchdog must share a declared endpoint");
        for (size_t i = 0; i < launch["argv"].size(); ++i) {
            const std::string p = "/launch/argv/" + std::to_string(i);
            if (launch["argv"][i].contains("when")) expression(definition, p + "/when");
            for (size_t j = 0; j < launch["argv"][i]["args"].size(); ++j) expression(definition, p + "/args/" + std::to_string(j));
        }
        std::set<std::string> environment;
        const auto entries = launch["environment"].value("set", json::array());
        for (size_t i = 0; i < entries.size(); ++i) {
            const std::string p = "/launch/environment/set/" + std::to_string(i);
            if (!environment.insert(entries[i]["name"]).second) error("duplicate_value", p + "/name", "Duplicate environment variable");
            expression(definition, p + "/value");
        }
        if (launch["program"].contains("missing_error")) expression(definition, "/launch/program/missing_error");
        const auto artifacts = doc.value("artifacts", json::object());
        for (const auto& [name, artifact] : artifacts.items()) {
            const std::string p = "/artifacts/" + escape(name);
            if (artifact.value("cache_resolution", "default") == "sentinel_directory" || artifact.value("import_resolution", "default") == "sentinel_directory") error("unsupported_feature", p, "Sentinel artifact adapter not installed");
            if (artifact.contains("missing_error")) expression(definition, p + "/missing_error");
            else error("required", p + "/missing_error", "Load artifact error is required");
            if (artifact.contains("hook") || artifact["strategy"] == "hook") error("unknown_hook", p + "/hook", "Artifact hook not installed");
        }
        auto& install = doc["install"];
        if (install["strategy"] == "hook" || install.contains("hook")) error("unknown_hook", "/install/hook", "Install hook not installed");
        install["cases"] = install.value("cases", json::array());
        if (install["strategy"] == "github_release" && install["cases"].empty()) error("required", "/install/cases", "Release cases required");
        for (size_t i = 0; i < install["cases"].size(); ++i) {
            const std::string p = "/install/cases/" + std::to_string(i);
            if (install.value("supports_split_archive", false) && install["cases"][i].contains("sha256")) error("unsupported_feature", p + "/sha256", "Combined split archive digest is not installed");
            expression(definition, p + "/filename");
            if (install["cases"][i].contains("version_override")) expression(definition, p + "/version_override");
            for (size_t j = 0; j < i; ++j) {
                bool overlap = true;
                for (const auto* axis : {"backend", "os", "arch"}) {
                    const auto& left = install["cases"][i]; const auto& right = install["cases"][j];
                    if (!left.contains(axis) || !right.contains(axis)) continue;
                    bool intersects = false;
                    for (const auto& item : left[axis]) intersects |= std::find(right[axis].begin(), right[axis].end(), item) != right[axis].end();
                    overlap &= intersects;
                }
                if (overlap) error("selector_overlap", p, "Install selectors overlap");
            }
        }
        const auto install_errors = install.value("errors", json::object());
        for (const auto& [key, value] : install_errors.items()) expression(definition, "/install/errors/" + key);
        const auto hooks = doc.value("hooks", json::object());
        for (const auto& [stage, selection] : hooks.items()) error("unknown_hook", "/hooks/" + stage + "/id", "Hook implementation is not installed");
        for (size_t i = 0; i < doc.value("rationale", json::array()).size(); ++i) {
            const std::string p = "/rationale/" + std::to_string(i) + "/subject";
            const std::string subject = doc["rationale"][i]["subject"];
            pointer_valid(subject, p, true);
            try { if (!doc.contains(json::json_pointer(subject))) error("invalid_reference", p, "Rationale subject absent"); } catch (...) {}
        }
        for (size_t i = 0; i < doc["operations"].size(); ++i) {
            const auto& record = doc["operations"][i];
            const std::string p = "/operations/" + std::to_string(i);
            OperationProgram program;
            BackendOperation operation = BackendOperation::Count;
            if (!parse_backend_operation(record["operation"].get<std::string>(), operation)) error("unknown_operation", p + "/operation", "Unknown operation");
            const std::string delivery = record["delivery"];
            const auto mode = delivery == "buffered" ? OperationDelivery::Buffered : delivery == "streaming" ? OperationDelivery::Streaming : OperationDelivery::Realtime;
            ModelTypeMask mask = 0;
            const std::map<std::string, ModelType> modes = {{"chat", ModelType::LLM}, {"embedding", ModelType::EMBEDDING}, {"reranking", ModelType::RERANKING}, {"transcription", ModelType::TRANSCRIPTION}, {"image", ModelType::IMAGE}, {"tts", ModelType::TTS}, {"audio-generation", ModelType::AUDIO_GENERATION}, {"classification", ModelType::CLASSIFICATION}, {"mesh", ModelType::MESH}};
            for (const auto& value : record["model_modes"]) {
                const auto name = value.get<std::string>();
                const auto found = modes.find(name);
                const auto bit = name == "*" ? k_all_operation_model_types : found == modes.end() ? 0 : operation_model_type_mask(found->second);
                if (!bit) error("invalid_value", p + "/model_modes", "Unknown model mode");
                if (mask & bit) error("operation_overlap", p + "/model_modes", "Duplicate mode");
                mask |= bit;
            }
            program.description = {operation, mode, mask, OperationProvenance::Implemented};
            if (!mask || !valid_operation_delivery(operation, mode)) error("unsupported_feature", p, "Invalid operation/delivery/modes");
            if (record.contains("transport") == record.contains("handler")) error("invalid_relationship", p, "Exactly one transport or handler required");
            if (record.contains("handler")) error("unknown_handler", p + "/handler/id", "Operation hook not installed");
            if (record.contains("transport")) {
                const auto& transport = record["transport"];
                program.kind = transport["kind"]; program.path = transport["path"]; program.port = transport["port"];
                if (program.port != launch["readiness"]["port"].get<std::string>()) error("unsupported_feature", p + "/transport/port", "Multiple HTTP service ports are not installed");
                if (!ports.count(program.port)) error("invalid_reference", p + "/transport/port", "Undeclared transport port");
                if (program.path.empty() || program.path[0] != '/' || program.path.find("://") != std::string::npos) error("invalid_value", p + "/transport/path", "Relative endpoint path required");
                program.timeout_seconds = transport["timeout"] == "global" ? 0 : transport["timeout"].get<long>();
                program.codec = transport.value("response_codec", transport.value("stream_codec", ""));
                const bool json_transport = program.kind == "http_json" && mode == OperationDelivery::Buffered && operation <= BackendOperation::Responses && program.codec == "json.v1" && !transport.contains("stream_codec");
                const bool sse_transport = program.kind == "http_sse" && mode == OperationDelivery::Streaming && operation <= BackendOperation::Responses && program.codec == "openai_sse.v1" && !transport.contains("response_codec");
                const bool bytes_transport = program.kind == "http_bytes" && mode == OperationDelivery::Buffered && operation == BackendOperation::AudioGeneration && program.codec == "raw_bytes.v1" && !transport.contains("response_codec");
                if (transport["method"] != "POST" || !(json_transport || sse_transport || bytes_transport)) error("unsupported_feature", p + "/transport", "Executable transport/codec/adapter combination not installed");
            }
            for (const auto& previous : definition.operations) if (previous.description.operation == operation && previous.description.delivery == mode && (previous.description.model_types & mask)) error("operation_overlap", p, "Overlapping operation cells");
            const auto& request = record["request"];
            program.identity = request["base"] == "identity";
            program.omit_missing = request.value("omit_missing", false);
            program.remove = request.value("remove", std::vector<std::string>{});
            for (const auto& path : program.remove) pointer_valid(path, p + "/request/remove", false);
            std::vector<std::string> destinations;
            const auto assignments = request.value("set", json::object());
            for (const auto& [destination, value] : assignments.items()) {
                const std::string path = p + "/request/set/" + escape(destination);
                pointer_valid(destination, path, false);
                for (const auto& prior : destinations) if (destination.rfind(prior + "/", 0) == 0 || prior.rfind(destination + "/", 0) == 0) error("mapping_overlap", path, "Overlapping destinations");
                destinations.push_back(destination);
                program.set.emplace_back(destination, compile(value, path));
            }
            program.formats = record.value("formats", std::vector<std::string>{});
            const auto formats = record.value("format_metadata", json::object());
            for (const auto& [format, metadata] : formats.items()) program.format_metadata.emplace(format, FormatMetadata{metadata.value("content_type", ""), metadata.value("headers", std::map<std::string, std::string>{})});
            if (operation == BackendOperation::AudioGeneration && program.formats.empty()) error("required", p + "/formats", "Media formats required");
            definition.operations.push_back(std::move(program));
        }
    }
    std::string id;
private:
    const DefinitionSource& source;
    DefinitionValidationResult& result;
    size_t nodes = 0;
};
}

DefinitionValidationResult BackendDefinitionStore::validate(const DefinitionSource& source) const {
    DefinitionValidationResult result;
    Validator validator(source, result);
    if (source.text.size() > 1024 * 1024) { validator.error("limit_exceeded", "", "Definition exceeds 1 MiB"); return result; }
    try {
        struct ParseContainer {
            std::string pointer, key;
            size_t index = 0;
            bool array;
            std::set<std::string> keys;
        };
        std::vector<ParseContainer> containers;
        auto next_pointer = [&] {
            if (containers.empty()) return std::string();
            const auto& parent = containers.back();
            return parent.pointer + "/" + (parent.array ? std::to_string(parent.index) : escape(parent.key));
        };
        auto duplicate_keys = [&](int, json::parse_event_t event, json& value) {
            if (event == json::parse_event_t::object_start || event == json::parse_event_t::array_start) {
                containers.push_back({next_pointer(), "", 0, event == json::parse_event_t::array_start, {}});
            } else if (event == json::parse_event_t::key) {
                auto& parent = containers.back();
                parent.key = value.get<std::string>();
                if (!parent.keys.insert(parent.key).second) validator.error("duplicate_key", next_pointer(), "Duplicate object key");
            } else if (event == json::parse_event_t::object_end || event == json::parse_event_t::array_end) {
                containers.pop_back();
                if (!containers.empty() && containers.back().array) ++containers.back().index;
            } else if (event == json::parse_event_t::value && !containers.empty() && containers.back().array) {
                ++containers.back().index;
            }
            return true;
        };
        auto definition = std::make_shared<BackendDefinition>();
        definition->records = json::parse(source.text, duplicate_keys);
        validator.id = definition->records.is_object() && definition->records.value("id", json()).is_string() ? definition->records["id"].get<std::string>() : "";
        static const json schema = json::parse(k_backend_definition_schema);
        validator.schema(definition->records, schema, schema, "");
        if (result.diagnostics.empty()) {
            definition->id = definition->records["id"];
            definition->recipe = definition->records["recipe"];
            definition->origin = source.origin;
            validator.relationships(*definition);
            if (result.diagnostics.empty()) result.definition = std::move(definition);
        }
    } catch (const json::parse_error& error) {
        validator.error("syntax_error", "", "JSON syntax error at byte " + std::to_string(error.byte));
    } catch (const std::exception&) {
        validator.error("invalid_definition", "", "Definition cannot be compiled");
    }
    for (auto& error : result.diagnostics) error.definition = validator.id;
    std::sort(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& left, const auto& right) { return std::tie(left.file, left.pointer, left.code) < std::tie(right.file, right.pointer, right.code); });
    return result;
}

}
