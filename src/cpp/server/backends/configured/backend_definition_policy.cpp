#include "backend_definition_policy_internal.h"

#include "lemon/utils/custom_args.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace lemon::backends::v2 {
namespace {
using Kind = MutableFragmentKind;
using Type = MutableValueType;
using Phase = ReferencePhase;

const Json& optional_array(const Json& value, const std::string& key) {
    static const Json empty = Json::array();
    const auto found = value.find(key);
    return found == value.end() ? empty : *found;
}

void require_policy(bool condition, const std::string& code = "invalid_mutable_fragment") {
    if (!condition) throw AuthorizationFailure{code, {{}, ""}};
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool identifier(const std::string& text, bool flag = false) {
    if (text.empty() || (flag && (text.size() < 2 || text[0] != '-'))) return false;
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (flag) {
        const auto prefix = text.rfind("--", 0) == 0 ? 2u : 1u;
        if (prefix == text.size() || !letter(text[prefix])) return false;
    } else if (!letter(text.front())) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

const Json* value_at(const Json& doc, const DefinitionTarget& target, Budget& budget) {
    const auto bytes = checked_sum(target.path.size(), checked_sum(target.at.size(), target.match ? target.match->value.size() : 0));
    budget.work(bytes, 1);
    budget.keep(checked_product(checked_sum(bytes, 128), 8));
    const Json::json_pointer path(target.path);
    if (!doc.contains(path)) return nullptr;
    const auto* result = &doc.at(path);
    if (target.match) {
        if (!result->is_array()) return nullptr;
        const auto field = target.match->field == RecordKey::Field::Id ? "id" : "name";
        const auto found = std::find_if(result->begin(), result->end(), [&](const auto& record) {
            return record.is_object() && record.value(field, Json()) == target.match->value;
        });
        if (found == result->end()) return nullptr;
        result = &*found;
        const Json::json_pointer at(target.at);
        if (!result->contains(at)) return nullptr;
        result = &result->at(at);
    }
    return result;
}

std::vector<std::string> fields(Kind kind) {
    if (kind == Kind::OptionPresentationV1) return {"name", "cli_flag", "help", "group"};
    if (kind == Kind::OptionDefaultV1) return {"default_value", "enum", "minimum", "maximum", "unset_values"};
    if (kind == Kind::ArgvSpellingsV1) return {"flag", "aliases", "value"};
    return {};
}

std::string collection_path(Kind kind) {
    if (kind == Kind::CustomDefaultsV1) return "/launch/custom_args/defaults";
    if (kind == Kind::EnvironmentEntriesV1) return "/launch/environment/set";
    if (kind == Kind::SupportRowsV1) return "/metadata/support";
    if (kind == Kind::InstallCasesV1) return "/install/cases";
    return "";
}

std::string schema_fragment(Kind kind) {
    if (kind == Kind::OptionPresentationV1 || kind == Kind::OptionDefaultV1) return "Option";
    if (kind == Kind::ArgvSpellingsV1) return "ArgvRecord";
    if (kind == Kind::CustomDefaultsV1) return "CustomArgDefault";
    if (kind == Kind::EnvironmentEntriesV1) return "EnvironmentSet";
    if (kind == Kind::SupportRowsV1) return "Support";
    if (kind == Kind::InstallCasesV1) return "InstallCase";
    return "";
}

void bounded_value(const Json& value, const MutableFragmentRule& rule, Budget& budget) {
    budget.work(0, 1);
    const auto bytes = value.is_string() ? value.get_ref<const std::string&>().size() : 0;
    budget.keep(checked_product(checked_sum(sizeof(Json) + 64, bytes), 16));
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        budget.work(text.size(), 0);
        require_policy(text.size() <= rule.maximum_length && text.find('\0') == std::string::npos);
    }
    if (value.is_structured()) {
        require_policy(value.size() <= rule.maximum_records);
        for (const auto& [key, child] : value.items()) {
            budget.work(key.size(), 1);
            budget.keep(checked_product(checked_sum(key.size(), 64), 16));
            require_policy(key.size() <= rule.maximum_length);
            bounded_value(child, rule, budget);
        }
    }
}

bool scalar_type(const Json& value, Type type) {
    if (type == Type::Boolean) return value.is_boolean();
    if (type == Type::Integer) return value.is_number_integer();
    if (type == Type::Number) return value.is_number();
    return type == Type::String && value.is_string();
}

Type scalar_kind(const Json& value) {
    if (value.is_boolean()) return Type::Boolean;
    if (value.is_number_integer()) return Type::Integer;
    if (value.is_number()) return Type::Number;
    require_policy(value.is_string(), "typed_reference_mismatch");
    return Type::String;
}

bool compatible(Type actual, Type expected) { return actual == expected || (actual == Type::Integer && expected == Type::Number); }

std::string type_name(Type type) {
    if (type == Type::Boolean) return "boolean";
    if (type == Type::Integer) return "integer";
    if (type == Type::Number) return "number";
    return "string";
}

Type expression_type(const Json& expression, const MutableFragmentRule& rule, const Json& doc,
    Phase use, Budget& budget) {
    budget.work(0, 1);
    if (!expression.is_object() || expression.contains("literal"))
        return scalar_kind(expression.is_object() ? expression.at("literal") : expression);
    if (expression.contains("ref")) {
        const auto& path = expression.at("ref").get_ref<const std::string&>();
        budget.work(path.size(), checked_sum(optional_array(doc.at("metadata"), "options").size(), doc.at("launch").at("ports").size()));
        budget.keep(checked_product(checked_sum(path.size(), 128), 4));
        const auto known = typed_data_reference(path, doc);
        const auto allowed = std::find_if(rule.references.begin(), rule.references.end(), [&](const auto& reference) {
            return reference.path == path;
        });
        require_policy(known && allowed != rule.references.end() && known->type == allowed->type &&
            known->phase == allowed->phase && static_cast<unsigned>(known->phase) <= static_cast<unsigned>(use), "typed_reference_mismatch");
        require_policy(!expression.contains("type") || expression.at("type") == type_name(known->type), "typed_reference_mismatch");
        require_policy(!expression.contains("default") || scalar_type(expression.at("default"), known->type), "typed_reference_mismatch");
        return known->type;
    }
    if (expression.contains("option")) {
        const auto& id = expression.at("option").get_ref<const std::string&>();
        const auto allowed = std::find_if(rule.references.begin(), rule.references.end(), [&](const auto& reference) {
            return reference.path == "/options/" + id;
        });
        budget.work(id.size(), optional_array(doc.at("metadata"), "options").size());
        budget.keep(checked_product(checked_sum(id.size(), 128), 4));
        const auto known = typed_data_reference("/options/" + id, doc);
        require_policy(known && allowed != rule.references.end() && known->type == allowed->type && allowed->phase == Phase::Input,
            "typed_reference_mismatch");
        require_policy(!expression.contains("type") || expression.at("type") == type_name(known->type), "typed_reference_mismatch");
        require_policy(!expression.contains("default") || scalar_type(expression.at("default"), known->type), "typed_reference_mismatch");
        return known->type;
    }
    const auto& op = expression.begin().key();
    const auto& args = expression.begin().value();
    std::vector<Type> types;
    const auto append = [&](const Json& child) {
        budget.keep(sizeof(Type));
        types.push_back(expression_type(child, rule, doc, use, budget));
    };
    if (args.is_array()) for (const auto& child : args) append(child);
    else append(args);
    if (op == "string") return Type::String;
    if (op == "present") return Type::Boolean;
    if (op == "eq" || op == "gt") {
        require_policy(types.size() == 2 && (compatible(types[0], types[1]) || compatible(types[1], types[0])), "typed_reference_mismatch");
        if (op == "gt") require_policy(types[0] == Type::Number || types[0] == Type::Integer, "typed_reference_mismatch");
        return Type::Boolean;
    }
    if (op == "all" || op == "any" || op == "not") {
        for (const auto type : types) require_policy(type == Type::Boolean, "typed_reference_mismatch");
        return Type::Boolean;
    }
    if (op == "when") {
        require_policy(types.size() == 2 && types[0] == Type::Boolean, "typed_reference_mismatch");
        return types[1];
    }
    if (op == "coalesce") {
        require_policy(!types.empty(), "typed_reference_mismatch");
        for (const auto type : types) require_policy(compatible(type, types[0]), "typed_reference_mismatch");
        return types[0];
    }
    require_policy(op == "concat" || op == "parent_path" || op == "path_join", "typed_reference_mismatch");
    for (const auto type : types) require_policy(type == Type::String, "typed_reference_mismatch");
    return Type::String;
}

void expression(const Json& value, Type expected, const MutableFragmentRule& rule, const Json& doc,
    Phase use, Budget& budget) {
    const auto checked = check_fragment(value, "Expression", budget);
    require_policy(checked.entries.empty() && !checked.truncated);
    require_policy(compatible(expression_type(value, rule, doc, use, budget), expected), "typed_reference_mismatch");
}

void domain(const MutableFragmentRule& rule, const std::string& name, const std::string& value) {
    const auto found = std::find_if(rule.domains.begin(), rule.domains.end(), [&](const auto& item) { return item.first == name; });
    require_policy(found != rule.domains.end() && contains(found->second, value), "mutable_domain_escape");
}

void flag_list(const Json& list) {
    require_policy(list.is_array());
    std::set<std::string> seen;
    for (const auto& value : list) require_policy(value.is_string() && identifier(value.get<std::string>(), true) && seen.insert(value.get<std::string>()).second);
}

void validate_record(const Json& record, const MutableFragmentRule& rule, const Json& doc, Budget& budget) {
    const auto checked = check_fragment(record, schema_fragment(rule.kind), budget);
    require_policy(checked.entries.empty() && !checked.truncated);
    bounded_value(record, rule, budget);
    if (rule.kind == Kind::OptionPresentationV1 || rule.kind == Kind::OptionDefaultV1) {
        require_policy(identifier(record.at("name").get<std::string>()));
        if (record.contains("cli_flag")) require_policy(identifier(record.at("cli_flag").get<std::string>(), true));
        const auto type = record.at("value_type").get<std::string>();
        const auto expected = type == "boolean" ? Type::Boolean : type == "integer" ? Type::Integer : type == "number" ? Type::Number : Type::String;
        const auto check = [&](const Json& value) {
            require_policy(scalar_type(value, expected));
            if (value.is_number()) require_policy(std::isfinite(value.get<double>()) && value >= rule.minimum && value <= rule.maximum);
            if (record.contains("minimum")) require_policy(value.is_number() && value >= record.at("minimum"));
            if (record.contains("maximum")) require_policy(value.is_number() && value <= record.at("maximum"));
        };
        check(record.at("default_value"));
        for (const auto* name : {"enum", "unset_values"}) if (record.contains(name)) for (const auto& value : record.at(name)) check(value);
        if (record.contains("enum")) require_policy(std::find(record.at("enum").begin(), record.at("enum").end(), record.at("default_value")) != record.at("enum").end());
        for (const auto* name : {"minimum", "maximum"}) if (record.contains(name))
            require_policy(record.at(name).is_number() && record.at(name) >= rule.minimum && record.at(name) <= rule.maximum);
        if (record.contains("minimum") && record.contains("maximum")) require_policy(record.at("minimum") <= record.at("maximum"));
    }
    if (rule.kind == Kind::ArgvSpellingsV1 || rule.kind == Kind::CustomDefaultsV1) {
        require_policy(record.contains("flag") && identifier(record.at("flag").get<std::string>(), true));
        if (record.contains("aliases")) flag_list(record.at("aliases"));
        if (record.contains("when")) expression(record.at("when"), Type::Boolean, rule, doc, Phase::Launch, budget);
        if (rule.kind == Kind::ArgvSpellingsV1 && record.contains("value")) expression(record.at("value"), Type::String, rule, doc, Phase::Launch, budget);
        if (rule.kind == Kind::CustomDefaultsV1) {
            domain(rule, "stage", record.at("stage"));
            budget.keep(checked_product(checked_sum(record.at("args").get_ref<const std::string&>().size(), 1), 256));
            const auto tokens = utils::parse_custom_args(record.at("args").get_ref<const std::string&>());
            require_policy(!tokens.empty() && tokens.front() == record.at("flag"));
            const auto arguments = utils::build_custom_args_map(tokens);
            for (const auto& [flag, values] : arguments) require_policy(flag == record.at("flag"), "custom_default_flag_escape");
            const auto& reserved = doc.at("launch").at("custom_args").at("reserved");
            require_policy(std::find(reserved.begin(), reserved.end(), record.at("flag")) == reserved.end(), "custom_default_flag_escape");
            if (record.contains("when")) expression(record.at("when"), Type::Boolean, rule, doc,
                record.at("stage") == "runtime_options" ? Phase::RuntimeOptions : Phase::Launch, budget);
        }
    }
    if (rule.kind == Kind::EnvironmentEntriesV1) {
        const auto name = record.at("name").get<std::string>();
        require_policy(!name.empty() && ((name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_') &&
            std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }));
        domain(rule, "name", name);
        expression(record.at("value"), Type::String, rule, doc, Phase::Launch, budget);
    }
    if (rule.kind == Kind::SupportRowsV1) {
        domain(rule, "backend", record.at("backend"));
        for (const auto& [device, architectures] : record.at("devices").items()) {
            domain(rule, "device", device);
            for (const auto& architecture : architectures) domain(rule, "arch", architecture);
        }
        if (record.contains("arch_gates")) for (const auto& [arch, gate] : record.at("arch_gates").items()) {
            domain(rule, "arch", arch);
            for (const auto& channel : gate.at("channels")) domain(rule, "channel", channel);
        }
    }
    if (rule.kind == Kind::InstallCasesV1) {
        domain(rule, "repo", record.at("repo"));
        for (const auto* field : {"backend", "arch"}) if (record.contains(field))
            for (const auto& value : record.at(field)) domain(rule, field, value);
        expression(record.at("filename"), Type::String, rule, doc, Phase::InstallAsset, budget);
        if (record.contains("version_override")) expression(record.at("version_override"), Type::String, rule, doc, Phase::InstallRepository, budget);
    }
}

void validate_rule(const MutableFragmentRule& rule, const Json& doc, const Json& base, Budget& budget) {
    const auto* value = value_at(doc, rule.target, budget);
    require_policy(value != nullptr, "missing_mutable_fragment");
    bounded_value(*value, rule, budget);
    if (!fields(rule.kind).empty()) {
        validate_record(*value, rule, doc, budget);
        if (rule.kind == Kind::ArgvSpellingsV1 && contains(rule.fields, "value")) {
            const auto* original = value_at(base, rule.target, budget);
            if (original && original->contains("value") && original->at("value").is_object() && !original->at("value").contains("literal"))
                require_policy(value->value("value", Json()) == original->at("value"), "managed_source_changed");
        }
    } else if (rule.kind == Kind::InstallCasesV1 && rule.target.path == "/install/default_versions") {
        require_policy(value->is_object());
        for (const auto& [backend, version] : value->items()) {
            domain(rule, "backend", backend);
            require_policy(version.is_string() && !version.get_ref<const std::string&>().empty());
        }
        const auto* original = value_at(base, rule.target, budget);
        if (original) {
            for (const auto& [backend, version] : original->items()) require_policy(value->contains(backend) || rule.collection.remove, "collection_permission_denied");
            for (const auto& [backend, version] : value->items()) require_policy(original->contains(backend) || rule.collection.add, "collection_permission_denied");
        }
        for (const auto& required : rule.required_records) require_policy(value->contains(required), "required_fragment_record_removed");
    } else if (!collection_path(rule.kind).empty()) {
        require_policy(value->is_array());
        std::set<std::string> identities;
        const auto key = rule.kind == Kind::EnvironmentEntriesV1 ? "name" : "id";
        for (const auto& record : *value) {
            validate_record(record, rule, doc, budget);
            require_policy(identities.insert(record.at(key)).second);
        }
        for (const auto& required : rule.required_records) require_policy(identities.count(required), "required_fragment_record_removed");
        const auto* before = value_at(base, rule.target, budget);
        if (before) {
            std::set<std::string> original;
            for (const auto& record : *before) original.insert(record.at(key));
            for (const auto& id : identities) require_policy(original.count(id) || rule.collection.add, "collection_permission_denied");
            for (const auto& id : original) require_policy(identities.count(id) || rule.collection.remove || (identities.empty() && rule.collection.clear), "collection_permission_denied");
        }
    } else if (rule.kind == Kind::ReservedFlagsV1) {
        flag_list(*value);
        for (const auto& argv : doc.at("launch").at("argv")) if (argv.contains("flag") && argv.value("reserve", "always") != "never") {
            for (const auto& flag : optional_array(argv, "aliases")) require_policy(std::find(value->begin(), value->end(), flag) != value->end(), "required_reservation_removed");
            require_policy(std::find(value->begin(), value->end(), argv.at("flag")) != value->end(), "required_reservation_removed");
        }
    } else if (rule.kind == Kind::ContextFloorsV1) {
        require_policy(value->is_object());
        static const std::vector<std::string> modes = {"llm", "embedding", "reranking", "transcription", "image", "tts", "mesh", "audio-generation", "classification"};
        for (const auto& [mode, floor] : value->items()) require_policy(contains(modes, mode) && floor.is_number_integer() && floor > 0 && floor >= rule.minimum && floor <= rule.maximum);
        const auto* original = value_at(base, rule.target, budget);
        if (original) for (const auto& [mode, floor] : original->items()) require_policy(value->contains(mode) || rule.collection.remove, "collection_permission_denied");
        for (const auto& mode : rule.required_records) require_policy(value->contains(mode), "required_fragment_record_removed");
    } else {
        const auto check = [&](const Json& item) { require_policy(item.is_string()); domain(rule, "value", item); };
        if (value->is_array()) for (const auto& item : *value) check(item);
        else if (value->is_object()) {
            for (const auto& [os, item] : value->items()) { require_policy(os == "windows" || os == "linux" || os == "macos"); check(item); }
        } else check(*value);
    }
}

bool same_target(const DefinitionTarget& a, const DefinitionTarget& b) {
    return a.path == b.path && a.at == b.at && bool(a.match) == bool(b.match) &&
        (!a.match || (a.match->field == b.match->field && a.match->value == b.match->value));
}
}

std::optional<TypedReferenceMetadata> typed_data_reference(const std::string& path, const Json& doc) {
    const auto result = [&](Type type, Phase phase) -> std::optional<TypedReferenceMetadata> { return TypedReferenceMetadata{type, phase}; };
    if (path == "/platform/os" || path == "/platform/arch" || path == "/platform/machine_arch") return result(Type::String, Phase::Input);
    if (doc.at("install").at("strategy") == "github_release" && (path == "/install/backend" || path == "/install/requested_version" || path == "/install/pinned_version")) return result(Type::String, Phase::InstallRepository);
    if (doc.at("install").at("strategy") == "github_release" && (path == "/install/version" || path == "/install/resolved_version")) return result(Type::String, Phase::InstallAsset);
    if (path.rfind("/artifacts/", 0) == 0 && doc.contains("artifacts") && doc.at("artifacts").contains(path.substr(11))) return result(Type::String, Phase::Artifacts);
    if (path == "/launch/backend" || path == "/launch/log_level" || path == "/launch/executable" || path == "/launch/program" || path == "/launch/executable_directory") return result(Type::String, Phase::Launch);
    if (path.rfind("/launch/ports/", 0) == 0) for (const auto& port : doc.at("launch").at("ports"))
        if (port.at("name") == path.substr(14)) return result(Type::Integer, Phase::Launch);
    if (path.rfind("/options/", 0) == 0) for (const auto& option : optional_array(doc.at("metadata"), "options")) if (option.at("id") == path.substr(9)) {
        const auto type = option.at("value_type");
        return result(type == "string" ? Type::String : type == "integer" ? Type::Integer : type == "number" ? Type::Number : Type::Boolean, Phase::Input);
    }
    const auto selected = [&](const std::string& stage, const std::string& id) {
        return doc.contains("hooks") && doc.at("hooks").contains(stage) && doc.at("hooks").at(stage).at("id") == id;
    };
    if (selected("launch", "llamacpp.prepare.v1")) {
        if (path == "/launch/hook/context_size") return result(Type::Integer, Phase::Prepare);
        for (const auto* field : {"hf_load", "use_mmproj", "mmproj_on_cpu", "use_draft_checkpoint", "embedding", "reranking"})
            if (path == std::string("/launch/hook/") + field) return result(Type::Boolean, Phase::Prepare);
    }
    if (selected("runtime_options", "llamacpp.runtime_options.v1"))
        for (const auto* field : {"dflash", "mtp", "draft"}) if (path == std::string("/runtime/") + field) return result(Type::Boolean, Phase::RuntimeOptions);
    return {};
}

Json fragment_rules_json(const std::vector<MutableFragmentRule>& rules) {
    auto result = Json::array();
    for (const auto& rule : rules) {
        auto references = Json::array();
        for (const auto& ref : rule.references) references.push_back({{"path", ref.path}, {"type", static_cast<unsigned>(ref.type)}, {"phase", static_cast<unsigned>(ref.phase)}});
        result.push_back({{"kind", static_cast<unsigned>(rule.kind)}, {"path", rule.target.path}, {"at", rule.target.at},
            {"match", rule.target.match ? Json{{"field", static_cast<unsigned>(rule.target.match->field)}, {"value", rule.target.match->value}} : Json()},
            {"fields", rule.fields}, {"add", rule.collection.add}, {"remove", rule.collection.remove}, {"clear", rule.collection.clear},
            {"maximum_records", rule.maximum_records}, {"maximum_length", rule.maximum_length}, {"minimum", rule.minimum}, {"maximum", rule.maximum},
            {"domains", rule.domains}, {"required_records", rule.required_records}, {"references", references}});
    }
    return result;
}

void audit_fragment_rules(const std::vector<MutableFragmentRule>& rules, const Json& declaration, Budget& budget) {
    require_policy(rules.size() <= 256);
    std::set<std::string> targets, leaves;
    for (const auto& rule : rules) {
        budget.work(0, 1);
        require_policy(rule.maximum_records > 0 && rule.maximum_records <= 1024 && rule.maximum_length > 0 && rule.maximum_length <= 65536 &&
            std::isfinite(rule.minimum) && std::isfinite(rule.maximum) && rule.minimum <= rule.maximum);
        require_policy(static_cast<unsigned>(rule.kind) <= static_cast<unsigned>(Kind::ContextFloorsV1));
        require_policy(rule.target.at.empty());
        const auto permitted = fields(rule.kind);
        if (!permitted.empty()) {
            const auto path = rule.kind == Kind::ArgvSpellingsV1 ? "/launch/argv" : "/metadata/options";
            require_policy(rule.target.path == path && rule.target.match && rule.target.match->field == RecordKey::Field::Id && !rule.target.match->value.empty());
            require_policy(!rule.fields.empty() && !rule.collection.add && !rule.collection.remove && !rule.collection.clear);
            std::set<std::string> seen;
            for (const auto& field : rule.fields) require_policy(contains(permitted, field) && seen.insert(field).second);
        } else {
            require_policy(!rule.target.match && rule.fields.empty());
            if (!collection_path(rule.kind).empty()) require_policy(rule.target.path == collection_path(rule.kind) || (rule.kind == Kind::InstallCasesV1 && rule.target.path == "/install/default_versions"));
            else if (rule.kind == Kind::ReservedFlagsV1) require_policy(rule.target.path == "/launch/custom_args/reserved");
            else if (rule.kind == Kind::ContextFloorsV1) require_policy(rule.target.path == "/launch/context_floor");
            else require_policy(rule.target.path == "/launch/backend/fixed" || rule.target.path == "/metadata/default_device" || rule.target.path == "/metadata/rocm_channels");
        }
        const auto key = rule.target.path + (rule.target.match ? ":" + rule.target.match->value : "");
        require_policy(targets.insert(key + ":" + std::to_string(static_cast<unsigned>(rule.kind))).second);
        for (const auto& field : rule.fields) require_policy(leaves.insert(key + "/" + field).second);
        std::set<std::string> reference_paths, domains;
        for (const auto& ref : rule.references) {
            budget.work(ref.path.size(), checked_sum(optional_array(declaration.at("metadata"), "options").size(), declaration.at("launch").at("ports").size()));
            budget.keep(checked_product(checked_sum(ref.path.size(), 128), 4));
            const auto known = typed_data_reference(ref.path, declaration);
            require_policy(known && known->type == ref.type && known->phase == ref.phase && reference_paths.insert(ref.path).second, "typed_reference_mismatch");
        }
        for (const auto& [name, values] : rule.domains) {
            require_policy(domains.insert(name).second && !values.empty() && values.size() <= rule.maximum_records);
            std::set<std::string> seen;
            for (const auto& value : values) require_policy(value.size() <= rule.maximum_length && seen.insert(value).second);
        }
        validate_rule(rule, declaration, declaration, budget);
    }
}

void verify_fragment_envelope(const BuiltinExportContract& contract, const Json& declaration, Budget& budget) {
    audit_fragment_rules(contract.mutable_fragments, declaration, budget);
    for (const auto& slot : contract.slots) {
        audit_fragment_rules(slot.mutable_fragments, declaration, budget);
        for (const auto& rule : slot.mutable_fragments) {
            const auto outer = std::find_if(contract.mutable_fragments.begin(), contract.mutable_fragments.end(), [&](const auto& item) {
                return same_target(item.target, rule.target) && item.kind == rule.kind;
            });
            require_policy(outer != contract.mutable_fragments.end(), "fragment_envelope_escape");
            require_policy(rule.maximum_records <= outer->maximum_records && rule.maximum_length <= outer->maximum_length &&
                rule.minimum >= outer->minimum && rule.maximum <= outer->maximum &&
                (!rule.collection.add || outer->collection.add) && (!rule.collection.remove || outer->collection.remove) && (!rule.collection.clear || outer->collection.clear), "fragment_envelope_escape");
            for (const auto& field : rule.fields) require_policy(contains(outer->fields, field), "fragment_envelope_escape");
            for (const auto& ref : rule.references) require_policy(std::any_of(outer->references.begin(), outer->references.end(), [&](const auto& item) {
                return item.path == ref.path && item.type == ref.type && item.phase == ref.phase;
            }), "fragment_envelope_escape");
            for (const auto& [name, values] : rule.domains) {
                for (const auto& value : values) domain(*outer, name, value);
            }
            for (const auto& required : outer->required_records) require_policy(contains(rule.required_records, required), "fragment_envelope_escape");
        }
    }
}

void validate_fragments(const std::vector<MutableFragmentRule>& rules, const Json& declaration, const Json& base, Budget& budget) {
    for (const auto& rule : rules) validate_rule(rule, declaration, base, budget);
}

void mask_fragments(Json& declaration, const std::vector<MutableFragmentRule>& rules, Budget& budget) {
    for (const auto& rule : rules) {
        budget.work(0, 1);
        const auto* found = value_at(declaration, rule.target, budget);
        if (!found) continue;
        auto& value = const_cast<Json&>(*found);
        if (!rule.fields.empty()) for (const auto& field : rule.fields) value.erase(field);
        else {
            auto path = Json::json_pointer(rule.target.path);
            const auto key = path.back();
            path.pop_back();
            declaration.at(path).erase(key);
        }
    }
}

void inspect_fragment_write(const std::vector<MutableFragmentRule>& rules, const NodePtr& root,
    const std::string& pointer, DefinitionEditKind kind, const Json* value, const SourceLocation& source, Budget& budget) {
    const auto check = [&](const MutableFragmentRule& rule, const std::string& at, DefinitionEditKind edit) {
        const auto& path = rule.target.path;
        if (at.size() < path.size() && (at.empty() || path.rfind(at + "/", 0) == 0) && edit != DefinitionEditKind::Merge)
            throw AuthorizationFailure{"fragment_subtree_replacement", source};
        if (!fields(rule.kind).empty()) {
            if (at == path) throw AuthorizationFailure{"fragment_subtree_replacement", source};
            if (at.rfind(path + "/", 0) == 0 && at.find('/', path.size() + 1) == std::string::npos &&
                edit != DefinitionEditKind::Merge) throw AuthorizationFailure{"fragment_subtree_replacement", source};
        }
        if (collection_path(rule.kind).empty() && rule.kind != Kind::ContextFloorsV1) return;
        if (at == path) {
            if (edit == DefinitionEditKind::Clear) require_policy(rule.collection.clear, "collection_permission_denied");
            else if (edit == DefinitionEditKind::Remove) require_policy(rule.collection.remove, "collection_permission_denied");
            else if (edit != DefinitionEditKind::Merge) throw AuthorizationFailure{"fragment_subtree_replacement", source};
        } else if (at.rfind(path + "/", 0) == 0) {
            const auto suffix = at.substr(path.size() + 1);
            if (suffix.find('/') != std::string::npos) return;
            if (edit == DefinitionEditKind::Remove) require_policy(rule.collection.remove, "collection_permission_denied");
            else if (edit == DefinitionEditKind::Clear) require_policy(rule.collection.clear, "collection_permission_denied");
            else if (edit == DefinitionEditKind::Add) require_policy(rule.collection.add, "collection_permission_denied");
            else if (edit == DefinitionEditKind::Replace && rule.kind != Kind::ContextFloorsV1 && rule.target.path != "/install/default_versions") throw AuthorizationFailure{"fragment_subtree_replacement", source};
        }
    };
    const auto walk = [&](const auto& self, const std::string& at, DefinitionEditKind edit, const Json* written) -> void {
        budget.work(at.size(), 1);
        for (const auto& rule : rules) check(rule, at, edit);
        if (written && written->is_object() && edit == DefinitionEditKind::Merge) for (const auto& [key, child] : written->items()) {
            const auto child_path = at + "/" + pointer_token(key);
            const auto before = effective_node(root, child_path, budget.limits);
            self(self, child_path, child.is_object() && before && before->kind == Node::Kind::Object ? DefinitionEditKind::Merge : DefinitionEditKind::Replace, &child);
        }
    };
    walk(walk, pointer, kind, value);
}

}
