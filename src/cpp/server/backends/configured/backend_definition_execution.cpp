#include "backend_definition_execution_internal.h"

#include <algorithm>
#include <type_traits>

namespace lemon::backends::execution {

#ifdef LEMONADE_BINDING_TEST
thread_local Counters counters;
#endif

void require(bool condition, const std::string& code, const std::string& pointer) {
    if (!condition) throw Failure{code, pointer};
}

void Budget::work(std::size_t bytes, std::size_t visits) {
    v2::charge(metrics.work_bytes, bytes, limits.work_bytes);
    v2::charge(metrics.work_visits, visits, limits.work_visits);
}

void Budget::keep(std::size_t bytes) {
    v2::charge(metrics.retained_bytes, bytes, limits.retained_bytes);
}

void Budget::copy(const Json& value) {
    const auto visit = [&](const auto& self, const Json& node) -> void {
        work(0);
        keep(sizeof(Json) + 64);
        if (node.is_string()) { const auto size = node.get_ref<const std::string&>().size(); keep(size); work(size, 0); }
        if (node.is_object()) for (const auto& [key, child] : node.items()) { keep(key.size() + 64); work(key.size(), 0); self(self, child); }
        else if (node.is_array()) for (const auto& child : node) self(self, child);
    };
    visit(visit, value);
}

void schema_walk(const Json& value, const Json& rule, const std::string& pointer,
    Budget& budget, const SchemaVisitor& visitor) {
    budget.work(pointer.size());
    if (rule.contains("$ref")) {
        const auto ref = rule["$ref"].get<std::string>();
        if (ref == "#/$defs/Expression" || ref == "#/$defs/Hook") { visitor(value, pointer, ref); return; }
        schema_walk(value, v2::schema_document().at(Json::json_pointer(ref.substr(1))), pointer, budget, visitor);
        return;
    }
    for (const auto* key : {"oneOf", "anyOf"}) if (rule.contains(key))
        for (const auto& branch : rule[key]) schema_walk(value, branch, pointer, budget, visitor);
    if (value.is_object()) for (const auto& [key, child] : value.items()) {
        const auto path = pointer + "/" + v2::pointer_token(key);
        if (rule.contains("properties") && rule["properties"].contains(key)) schema_walk(child, rule["properties"][key], path, budget, visitor);
        else if (rule.contains("additionalProperties") && rule["additionalProperties"].is_object()) schema_walk(child, rule["additionalProperties"], path, budget, visitor);
    }
    if (value.is_array() && rule.contains("items")) for (std::size_t i = 0; i < value.size(); ++i)
        schema_walk(value[i], rule["items"], pointer + "/" + std::to_string(i), budget, visitor);
}

namespace {
using Additions = std::vector<std::pair<std::string, Json>>;
Additions defaults(const Json& doc, Budget& budget) {
    Additions result;
    const auto add = [&](const std::string& path, const Json& value, const std::string& key, Json fallback) {
        if (value.contains(key)) return;
        const auto pointer = path + "/" + v2::pointer_token(key);
        budget.copy(fallback);
        budget.keep(pointer.size() + sizeof(Additions::value_type));
        result.emplace_back(pointer, std::move(fallback));
    };
    const auto& metadata = doc["metadata"];
    add("/metadata", metadata, "log_category", metadata["server_name"]);
    add("/metadata", metadata, "log_level_policy", "passthrough");
    add("/metadata", metadata, "options", Json::array());
    add("/install", doc["install"], "cases", Json::array());
    add("", doc, "artifacts", Json::object());
    add("/launch/environment", doc["launch"]["environment"], "set", Json::array());
    add("/launch/environment", doc["launch"]["environment"], "order", "preset_first");
    for (std::size_t i = 0; i < doc["launch"]["argv"].size(); ++i) {
        const auto& record = doc["launch"]["argv"][i];
        const auto p = "/launch/argv/" + std::to_string(i);
        add(p, record, "when", true);
        if (record.contains("flag")) { add(p, record, "reserve", "always"); add(p, record, "aliases", Json::array()); }
    }
    if (doc["launch"].contains("custom_args")) {
        const auto& custom = doc["launch"]["custom_args"];
        add("/launch/custom_args", custom, "defaults", Json::array());
        if (custom.contains("defaults")) for (std::size_t i = 0; i < custom["defaults"].size(); ++i) {
            const auto p = "/launch/custom_args/defaults/" + std::to_string(i);
            add(p, custom["defaults"][i], "when", true);
            add(p, custom["defaults"][i], "aliases", Json::array());
        }
    }
    if (doc.contains("artifacts")) for (const auto& [name, artifact] : doc["artifacts"].items())
        add("/artifacts/" + v2::pointer_token(name), artifact, "required_when", true);
    for (std::size_t i = 0; i < doc["operations"].size(); ++i) {
        const auto& op = doc["operations"][i];
        const auto p = "/operations/" + std::to_string(i);
        add(p + "/request", op["request"], "remove", Json::array());
        add(p + "/request", op["request"], "set", Json::object());
        add(p + "/request", op["request"], "omit_missing", false);
        if (op.contains("transport")) {
            const auto& transport = op["transport"];
            if (transport["kind"] == "http_json") add(p + "/transport", transport, "response_codec", "json.v1");
            if (transport["kind"] == "http_sse") add(p + "/transport", transport, "stream_codec", "openai_sse.v1");
        }
    }
    return result;
}

v2::Metrics measure(const Json& value, Budget& budget) {
    budget.work(0);
    v2::Metrics result{2, 1, 0};
    if (value.is_structured()) for (auto it = value.begin(); it != value.end(); ++it) {
        const auto child = measure(it.value(), budget);
        result.bytes = v2::checked_sum(result.bytes, child.bytes + (it == value.begin() ? 0 : 1));
        if (value.is_object()) result.bytes = v2::checked_sum(result.bytes, v2::string_bytes(it.key()) + 1);
        result.nodes = v2::checked_sum(result.nodes, child.nodes);
    }
    else result.bytes = value.is_string() ? v2::string_bytes(value.get_ref<const std::string&>()) : value.dump().size();
    budget.work(result.bytes, 0);
    return result;
}

ExecutionCompileLimits bounded(ExecutionCompileLimits value) {
    const ExecutionCompileLimits hard;
#define LEMON_CLAMP(field) value.field = std::min(value.field, hard.field)
    LEMON_CLAMP(normalized_bytes); LEMON_CLAMP(normalized_nodes); LEMON_CLAMP(expression_nodes);
    LEMON_CLAMP(expression_depth); LEMON_CLAMP(work_bytes); LEMON_CLAMP(work_visits);
    LEMON_CLAMP(retained_bytes); LEMON_CLAMP(diagnostics);
#undef LEMON_CLAMP
    return value;
}
}

std::string compilation_identity(const ResolvedDefinition& source, const Program& program, Budget& budget, unsigned revision) {
    budget.copy(program.normalized);
    budget.keep(v2::checked_sum(budget.metrics.normalized_bytes, 2048));
    budget.work(1024);
    Json hooks = Json::array();
    for (const auto& hook : program.hooks) {
        budget.keep(8192);
        budget.work(4096);
        Json inputs = Json::array();
        for (const auto& input : hook.inputs) inputs.push_back({{"fact", input.index},
            {"producer", input.producer ? Json(*input.producer) : Json(nullptr)}});
        hooks.push_back({{"grant", hook.grant_ordinal}, {"contract", static_cast<unsigned>(hook.contract)},
            {"owner_kind", static_cast<unsigned>(hook.slot.owner_kind)}, {"owner", hook.slot.owner_id},
            {"stage", hook.slot.stage}, {"id", hook.slot.hook_id}, {"order", hook.order},
            {"parameters", hook.parameter_digest}, {"owner_semantics", hook.owner_digest},
            {"policy", hook.policy_digest}, {"abi", hook.abi_revision}, {"implementation", hook.implementation_revision},
            {"effects", hook.effects}, {"inputs", inputs}, {"provides", hook.provides}, {"dependencies", hook.dependencies}});
    }
    Json identity = {{"hooks", hooks}, {"definition", source.definition_hash}, {"dependencies", source.dependency_digest},
        {"compiler", revision}, {"defaults", defaults_revision}, {"bindings", binding_revision},
        {"adapters", adapter_revision}, {"semantics", program.normalized}};
    DefinitionLimits limits;
    limits.work_bytes = budget.limits.work_bytes - budget.metrics.work_bytes;
    limits.work_nodes = budget.limits.work_visits - budget.metrics.work_visits;
    std::size_t retained = 0;
    v2::Budget hashing{limits, {}, retained};
    const auto result = v2::digest("lemonade.backend-compilation.v2\n", identity, hashing);
    budget.work(hashing.metrics.work_bytes, hashing.metrics.work_nodes);
    return result;
}

}

namespace lemon::backends {
using Json = nlohmann::json;

struct CompiledDefinition::Impl {
    Impl(std::shared_ptr<const AuthorizedDefinition> token, execution::Program&& staging,
        ExecutionCompileMetrics accounting, std::string compilation)
        : authorized(std::move(token)), program(std::move(staging)), metrics(accounting), identity(std::move(compilation)) {}
    const std::shared_ptr<const AuthorizedDefinition> authorized;
    const execution::Program program;
    const ExecutionCompileMetrics metrics;
    const std::string identity;
};

class DefinitionExecutionCompiler {
public:
    static ExecutionCompileResult compile(std::shared_ptr<const AuthorizedDefinition> authorized, ExecutionCompileLimits limits) {
        ExecutionCompileResult result;
        execution::Budget budget{execution::bounded(limits), {}};
        const auto error = [&](const std::string& code, const std::string& pointer) {
            if (budget.limits.diagnostics == 0) { result.diagnostics.truncated = true; return; }
            result.diagnostics.entries.push_back({code, authorized ? authorized->definition().key.id : "", pointer,
                code == "unsupported_execution_feature" ? "Execution feature is unavailable for the sealed authority" : "Execution compilation failed", {}, {}, {}});
        };
        try {
            if (!authorized) { error("invalid_authority", ""); return result; }
            const auto& source = authorized->definition();
            budget.keep(source.metrics.retained_logical_bytes);
            const auto additions = execution::defaults(source.declaration, budget);
            auto size = execution::measure(source.declaration, budget);
            for (const auto& [pointer, value] : additions) {
                const auto child = execution::measure(value, budget);
                const auto key = Json::json_pointer(pointer).back();
                size.bytes = v2::checked_sum(size.bytes, v2::checked_sum(child.bytes, v2::string_bytes(key) + 2));
                size.nodes = v2::checked_sum(size.nodes, child.nodes);
            }
            execution::require(size.bytes <= budget.limits.normalized_bytes && size.nodes <= budget.limits.normalized_nodes, "limit_exceeded", "");
            budget.metrics.normalized_bytes = size.bytes;
            budget.metrics.normalized_nodes = size.nodes;
            budget.copy(source.declaration);
            for (const auto& [pointer, value] : additions) { budget.copy(value); budget.keep(pointer.size() + 128); }
            budget.keep(sizeof(CompiledDefinition::Impl) + sizeof(CompiledDefinition));
            budget.work(size.bytes, size.nodes);
#ifdef LEMONADE_BINDING_TEST
            ++execution::counters.declaration_copies;
#endif
            execution::Program staging;
            staging.normalized = source.declaration;
            for (const auto& [pointer, value] : additions) staging.normalized[Json::json_pointer(pointer)] = value;
            execution::bind_options(staging, budget);
            execution::compile_hooks(*authorized, staging, budget);
            execution::compile_expressions(staging, budget);
            execution::compile_programs(staging, budget);
            const auto identity = execution::compilation_identity(source, staging, budget);
            auto impl = std::make_shared<const CompiledDefinition::Impl>(
                std::move(authorized), std::move(staging), budget.metrics, identity);
            static_assert(std::is_const_v<typename decltype(impl)::element_type>, "CompiledDefinitionImplIsActuallyConst");
            result.definition = std::shared_ptr<const CompiledDefinition>(new CompiledDefinition(std::move(impl)));
        } catch (const execution::Failure& failure) { error(failure.code, failure.pointer); }
        catch (const v2::LimitExceeded&) { error("limit_exceeded", ""); }
        catch (const std::exception&) { error("invalid_execution", ""); }
        return result;
    }
};

CompiledDefinition::CompiledDefinition(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
const AuthorizedDefinition& CompiledDefinition::authorized() const noexcept { return *impl_->authorized; }
const nlohmann::json& CompiledDefinition::normalized_declaration() const noexcept { return impl_->program.normalized; }
const std::vector<OptionBinding>& CompiledDefinition::option_bindings() const noexcept { return impl_->program.bindings; }
const std::vector<OperationDescription>& CompiledDefinition::operation_descriptions() const noexcept { return impl_->program.descriptions; }
const std::string& CompiledDefinition::compilation_id() const noexcept { return impl_->identity; }
const ExecutionCompileMetrics& CompiledDefinition::metrics() const noexcept { return impl_->metrics; }
const execution::Program& execution::Inspection::program(const CompiledDefinition& definition) { return definition.impl_->program; }

#ifdef LEMONADE_BINDING_TEST
std::shared_ptr<const CompiledDefinition> execution::Inspection::test_program(const CompiledDefinition& definition, Program program) {
    auto impl = std::make_shared<const CompiledDefinition::Impl>(definition.impl_->authorized,
        std::move(program), definition.impl_->metrics, definition.impl_->identity);
    return std::shared_ptr<const CompiledDefinition>(new CompiledDefinition(std::move(impl)));
}
#endif

ExecutionCompileResult compile_execution_definition(std::shared_ptr<const AuthorizedDefinition> authorized, const ExecutionCompileLimits& limits) {
    return DefinitionExecutionCompiler::compile(std::move(authorized), limits);
}

}
