#include "backend_definition_runner_internal.h"
#include "lemon/backends/backend_option_contract.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <algorithm>
#include <filesystem>
#include <limits>

namespace lemon::backends {
namespace {
const execution::Program& retained_program(const std::shared_ptr<const ExecutableDefinition>& executable) {
    if (!executable) throw std::invalid_argument("Runner requires a bound executable");
    return execution::Inspection::program(executable->compiled());
}
bool endpoint(const std::string& path) {
    return !path.empty() && path[0] == '/' && path.rfind("//", 0) != 0 &&
        path.find("://") == std::string::npos && path.find_first_of("\r\n") == std::string::npos && path.find('\0') == std::string::npos;
}
std::string scalar(const json& value) { return value.is_string() ? value.get<std::string>() : value.dump(); }
void remove_pointer(json& body, const std::string& path) {
    const json::json_pointer pointer(path);
    if (!body.contains(pointer)) return;
    auto& parent = body.at(pointer.parent_pointer());
    if (parent.is_object()) parent.erase(pointer.back());
    else if (parent.is_array()) parent.erase(parent.begin() + std::stoul(pointer.back()));
}
}

DefinitionRunner::DefinitionRunner(std::shared_ptr<const ExecutableDefinition> executable)
    : executable_(std::move(executable)), program_(retained_program(executable_)) {
    for (const auto& hook : program_.hooks) {
        if (hook.contract == typed::Contract::InstallContext || hook.contract == typed::Contract::Environment ||
            hook.contract == typed::Contract::System || hook.contract == typed::Contract::Artifacts)
            throw std::invalid_argument("Configured v2 capability consumer is unavailable");
    }
    if ((program_.install.strategy != "none" && program_.install.strategy != "external_only") ||
        std::find(program_.launch.prepare_order.begin(), program_.launch.prepare_order.end(), "install") != program_.launch.prepare_order.end() ||
        program_.launch.environment_preset != "inherit")
        throw std::invalid_argument("Configured v2 launch service is unavailable");
    for (const auto& artifact : program_.artifacts) if (artifact.records.at("strategy") != "resolved_path" ||
        artifact.records.value("absolute_checkpoint_fallback", false))
        throw std::invalid_argument("Configured v2 artifact consumer is unavailable");
    operation_bindings();
}
const json& DefinitionRunner::declaration() const { return program_.normalized; }
std::vector<OperationBinding> DefinitionRunner::operation_bindings() const {
    std::vector<OperationBinding> result;
    const auto& readiness = program_.launch.records.at("readiness");
    if (!endpoint(readiness.at("path")) || (readiness["timeout"] != "global" &&
        readiness["timeout"].get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<long>::max())))
        throw std::invalid_argument("Unsupported readiness adapter");
    for (const auto& op : program_.operations) {
        const auto& description = op.description;
        const auto operation = description.operation;
        const bool buffered = description.delivery == OperationDelivery::Buffered;
        const bool json_operation = operation <= BackendOperation::Reranking || operation == BackendOperation::Slots ||
            operation == BackendOperation::SlotAction || operation == BackendOperation::Tokenize;
        const bool installed = buffered ? json_operation && op.kind == "http_json" && op.codec == "json.v1" :
            description.delivery == OperationDelivery::Streaming && operation <= BackendOperation::Responses && op.kind == "http_sse" && op.codec == "openai_sse.v1";
        OperationHandlerIdentity handler;
        const bool handler_installed = operation_handler_identity(operation, description.delivery, handler);
        const bool port = std::any_of(program_.launch.ports.begin(), program_.launch.ports.end(), [&](const auto& item) { return item.name == op.port; });
        if (!installed || !description.model_types || (description.model_types & ~k_all_operation_model_types) ||
            !handler_installed ||
            !endpoint(op.path) || op.method != (operation == BackendOperation::Slots ? "GET" : "POST") ||
            !port || op.port != readiness["port"] || op.timeout_seconds > static_cast<std::uint64_t>(std::numeric_limits<long>::max()))
            throw std::invalid_argument("Configured v2 transport signature is not installed");
        result.push_back({description, handler.handler, handler.codec});
    }
    validate_operation_binding_signatures(result, false);
    return result;
}
std::vector<std::size_t> DefinitionRunner::slots(SlotOwnerKind kind, const std::string& owner, const std::string& stage) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < program_.hooks.size(); ++i) {
        const auto& hook = program_.hooks[i];
        if (hook.slot.owner_kind == kind && hook.slot.owner_id == owner && hook.slot.stage == stage) result.push_back(i);
    }
    std::sort(result.begin(), result.end(), [&](auto a, auto b) { return program_.hooks[a].order < program_.hooks[b].order; });
    return result;
}
const execution::OperationProgram& DefinitionRunner::cell(BackendOperation operation, OperationDelivery delivery, ModelType model_type) const {
    for (const auto& op : program_.operations) if (op.description.operation == operation && op.description.delivery == delivery &&
        (op.description.model_types & operation_model_type_mask(model_type))) return op;
    throw std::invalid_argument("Configured v2 operation cell is not bound");
}
runtime::Frame DefinitionRunner::request_frame(const ExecutionSession& session, const json& request) const {
    if (session.executable.get() != executable_.get()) throw std::logic_error("Foreign execution session");
    runtime::Frame frame(*executable_);
    frame.data = session.frame;
    frame.facts = session.facts;
    frame.produced = session.produced;
    frame.data.values["request"] = request;
    frame.facts.insert(typed::Fact::Request);
    return frame;
}
std::optional<json> DefinitionRunner::evaluate(const std::string& expression, const runtime::Frame& frame) const {
    return program_.expressions.at(expression).evaluate(frame.data, program_.bindings);
}
TransportRequest DefinitionRunner::map_request(BackendOperation operation, OperationDelivery delivery, ModelType model_type,
    const ExecutionSession& session, const json& request, std::optional<std::string_view> original_body, utils::RequestCancelToken cancel) const {
    runtime::check_cancelled(cancel);
    const auto& op = cell(operation, delivery, model_type);
    auto frame = request_frame(session, request);
    auto body = op.identity ? request : json::object();
    for (const auto& path : op.remove) remove_pointer(body, path);
    for (const auto& [path, expression] : op.set) {
        auto value = evaluate(expression, frame);
        if (!value && !op.omit_missing) throw std::invalid_argument("Missing request mapping value");
        if (value) body[json::json_pointer(path)] = std::move(*value);
    }
    const auto chain = slots(SlotOwnerKind::Operation, op.id, "request");
    for (const auto index : chain) body = invoke<typed::RequestTransform>(frame, index, SlotOwnerKind::Operation, op.id, "request", cancel,
        [&](auto target, auto&) { return target(std::move(body)); });
    runtime::check_cancelled(cancel);
    TransportRequest result{op.kind, op.method, op.codec, op.path, std::move(body), {}, static_cast<long>(op.timeout_seconds)};
    if (original_body && op.identity && op.remove.empty() && op.set.empty() && chain.empty()) result.original_body = std::string(*original_body);
    return result;
}
json DefinitionRunner::map_response(BackendOperation operation, ModelType model_type, const ExecutionSession& session,
    const json& request, json response, utils::RequestCancelToken cancel) const {
    if (response.contains("error")) return response;
    runtime::check_cancelled(cancel);
    const auto& op = cell(operation, OperationDelivery::Buffered, model_type);
    auto frame = request_frame(session, request);
    frame.facts.insert(typed::Fact::Response);
    for (const auto index : slots(SlotOwnerKind::Operation, op.id, "response")) response = invoke<typed::ResponseTransform>(
        frame, index, SlotOwnerKind::Operation, op.id, "response", cancel,
        [&](auto target, auto&) { return target(std::move(response), request, session.model_name); });
    runtime::check_cancelled(cancel);
    return response;
}

void DefinitionRunner::defaults(runtime::Frame& frame, const std::string& stage) const {
    if (!program_.launch.custom_args_slot) return;
    const auto& name = program_.bindings.at(*program_.launch.custom_args_slot).public_name;
    const auto existing = frame.data.options.find(name);
    if (existing != frame.data.options.end() && existing->is_null()) return;
    if (existing != frame.data.options.end() && *existing == "" && frame.explicit_options.count(name)) return;
    std::string text = existing == frame.data.options.end() ? "" : existing->get<std::string>();
    for (const auto& item : program_.launch.defaults) {
        if (item.stage != stage || (item.respect_merge_args && !frame.data.options.value("merge_args", true))) continue;
        if (!evaluate(item.condition, frame).value_or(false).get<bool>()) continue;
        text = utils::append_runtime_arg_defaults(text, {{item.args, item.flag, item.aliases}});
    }
    frame.data.options[name] = text;
    frame.data.values["options"] = frame.data.options;
}
void DefinitionRunner::resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const {
    runtime::Frame frame(*executable_);
    frame.data.options = options.to_json();
    for (const auto& binding : program_.bindings) {
        if (options.has_explicit_option(binding.public_name)) {
            frame.data.options[binding.public_name] = options.get_explicit_option(binding.public_name);
            frame.explicit_options.insert(binding.public_name);
        } else if (!frame.data.options.contains(binding.public_name)) frame.data.options[binding.public_name] = binding.contract.at("default_value");
    }
    frame.data.values["options"] = frame.data.options;
    frame.facts = {typed::Fact::ModelLabels, typed::Fact::DraftCheckpoint};
    for (const auto index : slots(SlotOwnerKind::Root, "runtime_options", "runtime_options")) {
        const auto facts = invoke<typed::RuntimeOptionsHook>(frame, index, SlotOwnerKind::Root, "runtime_options", "runtime_options", {},
            [&](auto target, auto&) { return target({model.labels, model.checkpoint("draft")}); });
        frame.data.values["model"]["dflash"] = facts.dflash;
        frame.data.values["model"]["mtp"] = facts.mtp;
    }
    defaults(frame, "runtime_options");
    if (program_.launch.custom_args_slot) {
        const auto& name = program_.bindings.at(*program_.launch.custom_args_slot).public_name;
        if (frame.data.options.contains(name)) options.set_option(name, frame.data.options.at(name));
    }
}

PreparedExecution DefinitionRunner::prepare_load(const LoadContext& context, const BackendSpec& spec,
    LaunchServices& services, utils::RequestCancelToken cancel) const {
    if (spec.executable_definition.get() != executable_.get() || spec.definition ||
        spec.public_recipe != declaration()["recipe"] || spec.pin_recipe != spec.public_recipe)
        throw std::logic_error("Install definition/spec ownership mismatch");
    runtime::Frame frame(*executable_);
    auto& values = frame.data.values;
    auto& options = frame.data.options;
    options = context.options.to_json();
    for (const auto& binding : program_.bindings) {
        if (context.options.has_explicit_option(binding.public_name)) frame.explicit_options.insert(binding.public_name);
        const auto value = context.options.has_explicit_option(binding.public_name) ? context.options.get_explicit_option(binding.public_name) :
            context.options.has_option(binding.public_name) ? context.options.get_option(binding.public_name) : binding.contract.at("default_value");
        if (!value.is_null()) {
            const auto error = validate_option_contract(binding.contract, value);
            if (!error.empty()) throw std::invalid_argument("Invalid option " + binding.public_name + ": " + error);
        }
        options[binding.public_name] = value;
    }
    const auto host = services.platform();
    const auto& launch = program_.launch;
    const auto& records = launch.records;
    if (records.contains("requires")) {
        const auto& required = records["requires"];
        for (const auto& axis : {std::pair<std::string, std::string>{"os", host.os}, {"machine_arch", host.machine_arch}})
            if (required.contains(axis.first) && std::find(required[axis.first].begin(), required[axis.first].end(), axis.second) == required[axis.first].end())
                throw std::runtime_error(required.at("error"));
    }
    values = {{"options", options}, {"model", {{"name", context.model_name}, {"checkpoint", context.model_info.checkpoint()}}},
        {"platform", {{"os", host.os}, {"machine_arch", host.machine_arch}, {"rocm_arch", host.rocm_arch}}},
        {"artifacts", json::object()}, {"launch", {{"ports", json::object()}, {"log_level", context.log_level}}}};
    frame.facts = {typed::Fact::BackendOptions, typed::Fact::Platform, typed::Fact::VersionInputs,
        typed::Fact::ModelLabels, typed::Fact::DraftCheckpoint, typed::Fact::ModelArtifacts, typed::Fact::ContextSize};
    LaunchPlan plan;
    std::string backend;
    bool use_gpu = false;
    for (const auto& phase : launch.prepare_order) {
        runtime::check_cancelled(cancel);
        if (phase == "backend") {
            backend = launch.backend_slot ? options.at(program_.bindings.at(*launch.backend_slot).public_name).get<std::string>() :
                launch.fixed_backend.is_string() ? launch.fixed_backend.get<std::string>() : launch.fixed_backend.at(host.os).get<std::string>();
            const auto schedule = slots(SlotOwnerKind::Root, "backend", "backend");
            if (!schedule.empty()) {
                for (const auto index : schedule) {
                    const auto selected = invoke<typed::BackendSelectionOps>(frame, index, SlotOwnerKind::Root, "backend", "backend", cancel,
                        [&](const auto& target, auto& guard) {
                            runtime::SelectionServices selection(guard, services.supported_backends(spec.public_recipe));
                            auto result = target.select_backend({backend, options.value("rocm_channel", "stable"), services.supported_backends(spec.public_recipe)}, selection);
                            const auto device = target.validate_device({result.backend, options.value("device", "")}, selection);
                            if (!device.supported) throw std::invalid_argument(device.error);
                            return result;
                        });
                    backend = selected.backend; use_gpu = selected.use_gpu;
                }
            } else {
                if (backend.empty()) {
                    const auto supported = services.supported_backends(spec.public_recipe);
                    if (supported.empty()) throw std::runtime_error("No supported configured backend");
                    backend = supported.front();
                }
                services.validate_backend(spec.public_recipe, backend);
                use_gpu = backend != "cpu";
                frame.facts.insert(typed::Fact::SelectedBackend);
            }
            values["launch"]["backend"] = backend;
            if (context.backend_selected) context.backend_selected();
        } else if (phase == "hook") {
            auto context_size = options.contains("ctx_size") && options["ctx_size"].is_number_integer() && options["ctx_size"].get<std::int64_t>() > 0 ? options["ctx_size"].get<std::uint64_t>() : 0;
            const auto floor = launch.context_floor.find(context.model_info.type);
            if (floor != launch.context_floor.end()) context_size = std::max(context_size, floor->second);
            for (const auto index : slots(SlotOwnerKind::Root, "launch", "launch")) {
                const auto facts = invoke<typed::PrepareHook>(frame, index, SlotOwnerKind::Root, "launch", "launch", cancel,
                    [&](auto target, auto&) { return target({context.model_info.type, context.model_info.extra<bool>("hf_load", false), use_gpu, context_size,
                        context.model_info.resolved_path("main"), context.model_info.resolved_path("mmproj"), context.model_info.resolved_path("draft"),
                        context.model_info.checkpoint("draft"), context.model_info.labels}); });
                values["launch"]["hook"] = {{"hf_load", facts.hf_load}, {"embedding", facts.embedding}, {"reranking", facts.reranking},
                    {"use_mmproj", facts.use_mmproj}, {"mmproj_on_cpu", facts.mmproj_on_cpu}, {"use_draft_checkpoint", facts.use_draft_checkpoint}, {"context_size", facts.context_size}};
            }
        } else if (phase == "artifacts") {
            for (const auto& artifact : program_.artifacts) {
                const auto path = context.model_info.resolved_path(artifact.role);
                values["artifacts"][artifact.name] = path;
                const bool required = evaluate(artifact.required_when, frame).value_or(true).get<bool>();
                if (required && (path.empty() || (artifact.records.value("check", "exists") != "nonempty" && !services.exists(path))))
                    throw std::runtime_error(evaluate(artifact.missing_error, frame).value().get<std::string>());
            }
        } else if (phase == "custom_args") {
            if (launch.custom_args_slot) {
                const auto& value = options.at(program_.bindings.at(*launch.custom_args_slot).public_name);
                const auto text = value.is_null() ? "" : value.get<std::string>();
                const auto error = utils::validate_custom_args(text, records["custom_args"]["reserved"].get<std::set<std::string>>());
                if (!error.empty()) throw std::invalid_argument(records["custom_args"]["error_prefix"].get<std::string>() + error);
            }
        } else if (phase == "install") {
            if (program_.install.strategy == "external_only") throw std::logic_error("External-only backend cannot be downloaded");
            throw std::logic_error("Configured v2 installation consumer is unavailable");
        } else if (phase == "program") {
            if (launch.program_source == "external_or_managed" || program_.install.strategy == "external_only")
                plan.command.program = services.external_binary(spec, backend);
            if (plan.command.program.empty()) {
                if (program_.install.strategy == "external_only") throw std::runtime_error("External backend executable is required");
                plan.command.program = services.managed_binary(spec, backend);
            }
            if (plan.command.program.empty() || !services.exists(plan.command.program)) throw std::runtime_error("Configured backend executable is missing");
            values["launch"]["executable"] = plan.command.program;
            values["launch"]["executable_directory"] = std::filesystem::path(plan.command.program).parent_path().string();
            frame.facts.insert(typed::Fact::ResolvedProgram);
        } else if (phase == "ports") {
            for (const auto& port : launch.ports) {
                int number = 0;
                if (port.strategy == "offset") number = values["launch"]["ports"].at(port.base).get<int>() + port.offset;
                else if (port.strategy == "probe") {
                    int start = 8001;
                    for (unsigned attempt = 0; attempt < port.attempts; ++attempt) {
                        runtime::check_cancelled(cancel);
                        const int candidate = services.find_free_port(start);
                        if (candidate <= 0 || candidate > 65535 - static_cast<int>(port.span)) break;
                        unsigned free = 1;
                        while (free < port.span && services.find_free_port(candidate + free) == candidate + static_cast<int>(free)) ++free;
                        if (free == port.span) { number = candidate; break; }
                        start = candidate + 1;
                    }
                } else number = services.allocate_port();
                if (number <= 0 || number > 65535) throw std::runtime_error(port.error.empty() ? "Invalid configured port" : port.error);
                values["launch"]["ports"][port.name] = number;
            }
        }
        if (context.phase_prepared) context.phase_prepared(phase, values);
    }
    defaults(frame, "launch");
    std::string custom;
    if (launch.custom_args_slot) {
        const auto& value = options.at(program_.bindings.at(*launch.custom_args_slot).public_name);
        if (!value.is_null()) custom = value.get<std::string>();
    }
    std::set<std::string> reserved;
    if (records.contains("custom_args")) for (const auto& flag : records["custom_args"]["reserved"]) reserved.insert(flag);
    for (const auto& arg : launch.argv) {
        const bool enabled = evaluate(arg.condition, frame).value_or(true).get<bool>();
        if (arg.flag && (arg.reserve == "always" || (arg.reserve == "when_active" && enabled))) {
            reserved.insert(*arg.flag); reserved.insert(arg.aliases.begin(), arg.aliases.end());
        }
        if (!enabled) continue;
        if (arg.flag) plan.command.args.push_back(*arg.flag);
        for (const auto& expression : arg.values) plan.command.args.push_back(scalar(evaluate(expression, frame).value()));
    }
    const auto error = utils::validate_custom_args(custom, reserved);
    if (!error.empty()) throw std::invalid_argument(records.contains("custom_args") ? records["custom_args"]["error_prefix"].get<std::string>() + error : error);
    const auto tokens = utils::parse_custom_args(custom);
    plan.command.args.insert(plan.command.args.end(), tokens.begin(), tokens.end());
    plan.command.env = services.environment(launch.environment_preset, plan.command.program, backend, declaration()["metadata"]["log_category"]);
    for (const auto& assignment : launch.environment_set) {
        const auto value = evaluate(assignment.expression, frame).value().get<std::string>();
        auto found = std::find_if(plan.command.env.begin(), plan.command.env.end(), [&](const auto& item) { return item.first == assignment.name; });
        if (found == plan.command.env.end()) plan.command.env.emplace_back(assignment.name, value);
        else if (launch.environment_order != "set_first") found->second = value;
    }
    const auto& readiness = records["readiness"];
    plan.command.port = values["launch"]["ports"].at(readiness["port"].get<std::string>());
    plan.command.ready_endpoint = readiness["path"];
    plan.readiness_timeout_seconds = readiness["timeout"] == "global" ? services.default_timeout_seconds() : readiness["timeout"].get<long>();
    plan.watchdog_enabled = records["watchdog"]["policy"] == "standard";
    if (launch.working_directory.at(host.os) == "executable_directory") plan.working_directory = std::filesystem::path(plan.command.program).parent_path().string();
    const auto& levels = records["output"]["inherit_levels"];
    plan.output = {std::find(levels.begin(), levels.end(), context.log_level) != levels.end(), records["output"]["filter"].get<bool>()};
    runtime::check_cancelled(cancel);
    auto session = std::make_shared<const ExecutionSession>(ExecutionSession{executable_, frame.data, frame.facts, frame.produced, context.model_name});
    return {std::move(plan), std::move(session)};
}

}
