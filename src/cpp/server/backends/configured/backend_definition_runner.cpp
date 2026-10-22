#include "backend_definition_runner_internal.h"
#include "lemon/backends/backend_option_contract.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <algorithm>
#include <cctype>
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
bool valid_spec_aliases(const ExecutableDefinition& executable, const BackendSpec& spec) {
    const auto& key = executable.compiled().authorized().definition().key;
    return (!spec.public_recipe.empty() && !spec.pin_recipe.empty()) ||
        (key.space == DefinitionNamespace::Builtin && key.id == "llamacpp" && key.contract == 1 && spec.recipe == "llamacpp");
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
    if (program_.launch.environment_preset != "inherit" && program_.launch.environment_preset != "hook")
        throw std::invalid_argument("Configured v2 launch service is unavailable");
    for (const auto& artifact : program_.artifacts) if (artifact.records.value("absolute_checkpoint_fallback", false))
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
TransportRequest DefinitionRunner::map_stream(const std::string& path, ModelType model_type, const ExecutionSession& session,
    const std::string& body, bool sse, long timeout, utils::RequestCancelToken cancel) const {
    runtime::check_cancelled(cancel);
    if (session.executable.get() != executable_.get()) throw std::logic_error("Foreign execution session");
    const auto policy = program_.launch.records.value("direct_stream_policy", "strict");
    const bool legacy = policy == "llamacpp_legacy";
    const bool passthrough = legacy || policy == "bound_passthrough";
    BackendOperation operation = BackendOperation::Count;
    if (path == "/v1/chat/completions") operation = BackendOperation::ChatCompletion;
    if (path == "/v1/completions") operation = BackendOperation::Completion;
    if (path == "/v1/responses") operation = BackendOperation::Responses;
    if (operation == BackendOperation::Count) {
        if (!legacy || !endpoint(path)) throw std::invalid_argument("Streaming operation adapter is not bound");
        return {"http_sse", "POST", "openai_sse.v1", path, {}, body, timeout, sse};
    }
    const auto& op = cell(operation, OperationDelivery::Streaming, model_type);
    const auto parsed = json::parse(body, nullptr, false);
    if (passthrough && parsed.is_discarded()) return {op.kind, op.method, op.codec, op.path, {}, body,
        op.timeout_seconds ? static_cast<long>(op.timeout_seconds) : timeout, sse};
    auto mapped = map_request(operation, OperationDelivery::Streaming, model_type, session,
        parsed.is_discarded() ? json::object() : parsed, body, cancel);
    if (parsed.is_discarded() && !mapped.original_body) throw std::invalid_argument("Invalid JSON for mapped streaming operation");
    if (!mapped.timeout_seconds) mapped.timeout_seconds = timeout;
    if (passthrough) mapped.sse = sse;
    return mapped;
}
json DefinitionRunner::map_response(BackendOperation operation, ModelType model_type, const ExecutionSession& session,
    const json& request, json response, utils::RequestCancelToken cancel) const {
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
    const bool normalize = stage == "runtime_options" && program_.launch.records.at("custom_args").value("runtime_input", "preserve") == "string_or_empty";
    if (!normalize && existing != frame.data.options.end() && existing->is_null()) return;
    if (!normalize && existing != frame.data.options.end() && *existing == "" && frame.explicit_options.count(name)) return;
    std::string text = existing == frame.data.options.end() || (normalize && !existing->is_string()) ? "" : existing->get<std::string>();
    const auto merge_value = frame.data.options.value("merge_args", json(true));
    const bool normalized_merge = !merge_value.is_boolean() || merge_value.get<bool>();
    for (const auto& item : program_.launch.defaults) {
        if (item.stage != stage || (item.respect_merge_args && !(normalize ? normalized_merge : frame.data.options.value("merge_args", true)))) continue;
        if (!evaluate(item.condition, frame).value_or(false).get<bool>()) continue;
        text = utils::append_runtime_arg_defaults(text, {{item.args, item.flag, item.aliases}});
    }
    frame.data.options[name] = text;
    frame.data.values["options"] = frame.data.options;
}
void DefinitionRunner::resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const {
    const bool runtime_hook = !slots(SlotOwnerKind::Root, "runtime_options", "runtime_options").empty();
    const bool runtime_default = std::any_of(program_.launch.defaults.begin(), program_.launch.defaults.end(),
        [](const auto& item) { return item.stage == "runtime_options"; });
    const bool normalize = program_.launch.custom_args_slot &&
        program_.launch.records.at("custom_args").value("runtime_input", "preserve") != "preserve";
    if (!runtime_hook && !runtime_default && !normalize) return;
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
        frame.data.values["runtime"]["dflash"] = facts.dflash;
        frame.data.values["runtime"]["mtp"] = facts.mtp;
    }
    defaults(frame, "runtime_options");
    if (program_.launch.custom_args_slot) {
        const auto& name = program_.bindings.at(*program_.launch.custom_args_slot).public_name;
        if (frame.data.options.contains(name)) options.set_option(name, frame.data.options.at(name));
    }
}

InstallParams DefinitionRunner::install_params(const InstallContext& context, runtime::InstallFactSources sources) const {
    if (context.executable_definition.get() != executable_.get() || context.spec.executable_definition.get() != executable_.get() ||
        !valid_spec_aliases(*executable_, context.spec) || context.definition || context.spec.definition || context.public_recipe != declaration()["recipe"] ||
        context.pin_recipe != context.public_recipe || context.spec.effective_public_recipe() != context.public_recipe || context.spec.effective_pin_recipe() != context.pin_recipe)
        throw std::logic_error("Install definition/spec ownership mismatch");
    if (program_.install.strategy != "github_release") throw std::logic_error("Backend has no managed release installation");
    const auto& policy = program_.install.records;
    std::string backend = context.backend;
    if (backend == "rocm" && !slots(SlotOwnerKind::Root, "install_context", "install_context").empty()) backend += "-" + sources.rocm_channel();
    const auto unmanaged = policy.value("unmanaged_backends", std::vector<std::string>{});
    if (std::find(unmanaged.begin(), unmanaged.end(), backend) != unmanaged.end()) return {};
    const auto has_backend = [&](const std::string& value) {
        return std::any_of(program_.install.cases.begin(), program_.install.cases.end(), [&](const auto& record) {
            const auto found = record.selectors.find("backend");
            return found == record.selectors.end() || std::find(found->second.begin(), found->second.end(), value) != found->second.end();
        });
    };
    if (!has_backend(backend) && policy.contains("backend_fallback")) backend = policy.at("backend_fallback");
    const bool full = policy.value("repository_result", "repository_only") == "full_params";
    const bool early = policy.value("fact_timing", "selected_case") == "before_platform";
    runtime::Frame frame(*executable_);
    frame.facts = {typed::Fact::BackendOptions, typed::Fact::SelectedBackend, typed::Fact::Platform, typed::Fact::VersionInputs};
    frame.data.values = {{"platform", {{"os", context.platform.os}, {"machine_arch", context.platform.machine_arch}, {"arch", context.platform.rocm_arch}}},
        {"install", {{"backend", backend}, {"requested_version", context.requested_version}, {"pinned_version", context.pinned_version}}}};
    if (context.phase == InstallPhase::Asset || full) {
        frame.data.values["install"]["version"] = context.phase == InstallPhase::Repository ? context.pinned_version : context.resolved_version;
        frame.data.values["install"]["resolved_version"] = context.resolved_version;
    }
    const auto fail = [&](const std::string& axis) -> void {
        const auto& errors = policy.value("errors", json::object());
        const auto key = errors.contains(axis) ? axis : "no_case";
        if (errors.contains(key)) throw std::runtime_error(evaluate("/install/errors/" + key, frame).value().get<std::string>());
        throw std::invalid_argument("No matching install case: " + axis);
    };
    std::vector<const execution::InstallCase*> eligible;
    for (const auto& record : program_.install.cases) eligible.push_back(&record);
    const auto filter = [&](const std::string& axis, const std::string& value) {
        eligible.erase(std::remove_if(eligible.begin(), eligible.end(), [&](const auto* record) {
            const auto found = record->selectors.find(axis);
            return found != record->selectors.end() && std::find(found->second.begin(), found->second.end(), value) == found->second.end();
        }), eligible.end());
        if (eligible.empty()) fail(axis);
    };
    filter("backend", backend);
    for (const auto index : slots(SlotOwnerKind::Root, "backend", "backend"))
        invoke<typed::BackendSelectionOps>(frame, index, SlotOwnerKind::Root, "backend", "backend", {}, [&](const auto& target, auto& guard) {
            runtime::SelectionServices services(guard, {backend});
            return target.select_backend({backend, "stable", {backend}}, services);
        });
    const auto read_facts = [&](const execution::InstallCase& fact_case) {
        typed::InstallFactsInput input{context.phase == InstallPhase::Repository ? typed::InstallPhase::Repository : typed::InstallPhase::Asset,
            runtime::typed_platform(context.platform.os), backend, context.requested_version, context.pinned_version, context.resolved_version, {}};
        for (const auto& name : policy.value("repository_facts", std::vector<std::string>{}))
            input.repository_facts.push_back(name == "therock_version" ? typed::InstallFact::TheRockVersion :
                name == "rocm_asset_family" ? typed::InstallFact::RocmAssetFamily : typed::InstallFact::CudaArchitecture);
        for (const auto index : slots(SlotOwnerKind::Root, "install_context", "install_context")) {
            invoke<typed::InstallContextHook>(frame, index, SlotOwnerKind::Root, "install_context", "install_context", {}, [&](auto target, auto& guard) {
                typed::InstallFacts combined;
                bool invoked = false;
                const auto read = [&] {
                    runtime::InstallServices services(guard, input, sources);
                    const auto facts = target(input, services);
                    invoked = true;
                    auto& values = frame.data.values["install"];
                    if (facts.therock_version) { combined.therock_version = facts.therock_version; values["therock_version"] = *facts.therock_version; }
                    if (facts.rocm_asset_family) { combined.rocm_asset_family = facts.rocm_asset_family; values["rocm_asset_family"] = *facts.rocm_asset_family; }
                    if (facts.cuda_architecture) { combined.cuda_architecture = facts.cuda_architecture; values["cuda_architecture"] = *facts.cuda_architecture; }
                };
                const auto request = [&](const auto& self, const execution::Expression& expression) -> void {
                    using Kind = execution::Expression::Kind;
                    for (const auto& [name, fact] : std::vector<std::pair<std::string, typed::InstallFact>>{
                        {"/install/therock_version", typed::InstallFact::TheRockVersion}, {"/install/rocm_asset_family", typed::InstallFact::RocmAssetFamily},
                        {"/install/cuda_architecture", typed::InstallFact::CudaArchitecture}}) {
                        if (expression.reference == name && !frame.data.values.contains(json::json_pointer(name))) {
                            input.requested = {fact};
                            read();
                        }
                    }
                    if (expression.kind == Kind::When) {
                        self(self, expression.children.at(0));
                        if (expression.children.at(0).evaluate(frame.data, program_.bindings).value().template get<bool>()) self(self, expression.children.at(1));
                        return;
                    }
                    for (const auto& child : expression.children) {
                        self(self, child);
                        if (expression.kind == Kind::Coalesce && child.evaluate(frame.data, program_.bindings)) break;
                        if (expression.kind == Kind::All || expression.kind == Kind::Any) {
                            const bool value = child.evaluate(frame.data, program_.bindings).value().template get<bool>();
                            if (expression.kind == Kind::All && !value) break;
                            if (expression.kind == Kind::Any && value) break;
                        }
                    }
                };
                if (context.phase == InstallPhase::Asset || full) {
                    request(request, program_.expressions.at(fact_case.filename));
                    if (fact_case.version_override) request(request, program_.expressions.at(*fact_case.version_override));
                }
                if (!invoked) { input.requested.clear(); read(); }
                return combined;
            });
        }
    };
    if (early) read_facts(*eligible.front());
    filter("os", context.platform.os);
    filter("machine_arch", context.platform.machine_arch);
    filter("arch", context.platform.rocm_arch);
    const auto& selected = *eligible.front();
    if (!early) read_facts(selected);
    if (context.phase == InstallPhase::Repository && !full) return {selected.repo, "", "", ""};
    const auto filename = evaluate(selected.filename, frame);
    const auto version = selected.version_override ? evaluate(*selected.version_override, frame) : std::optional<json>("");
    if (!filename || !version) throw std::invalid_argument("Missing selected install fact");
    return {selected.repo, filename->get<std::string>(), version->get<std::string>(), selected.sha256.value_or("")};
}

runtime::Frame DefinitionRunner::management_frame(const std::string& backend) const {
    runtime::Frame frame(*executable_);
    frame.facts = {typed::Fact::BackendOptions, typed::Fact::Platform, typed::Fact::VersionInputs, typed::Fact::SelectedBackend};
    for (const auto index : slots(SlotOwnerKind::Root, "backend", "backend"))
        invoke<typed::BackendSelectionOps>(frame, index, SlotOwnerKind::Root, "backend", "backend", {}, [&](const auto& target, auto& guard) {
            runtime::SelectionServices services(guard, {backend});
            return target.select_backend({backend, "stable", {backend}}, services);
        });
    return frame;
}
std::string DefinitionRunner::resolve_version(const std::string& backend, const std::string& file_version, runtime::ProbeSources sources, PlatformInfo platform) const {
    auto frame = management_frame(backend);
    auto version = file_version;
    for (const auto index : slots(SlotOwnerKind::Root, "system", "system"))
        version = invoke<typed::SystemOps>(frame, index, SlotOwnerKind::Root, "system", "system", {}, [&](const auto& target, auto& guard) {
            runtime::SystemServices services(guard, {runtime::typed_platform(platform.os), backend, true}, sources);
            return target.resolve_version({backend, file_version}, services);
        });
    return version;
}
BackendOps::InstallCheck DefinitionRunner::check_install(const std::string& backend, bool found, runtime::ProbeSources sources, PlatformInfo platform) const {
    auto frame = management_frame(backend);
    typed::SystemInstallCheck result{found, ""};
    const typed::SystemInstallInput input{runtime::typed_platform(platform.os), backend, found};
    for (const auto index : slots(SlotOwnerKind::Root, "system", "system"))
        result = invoke<typed::SystemOps>(frame, index, SlotOwnerKind::Root, "system", "system", {}, [&](const auto& target, auto& guard) {
            runtime::SystemServices services(guard, input, sources);
            return target.check_install(input, services);
        });
    return {result.installed, result.error};
}

void DefinitionRunner::populate_metadata(ModelInfo& model) const {
    if (program_.artifacts.empty()) return;
    const auto path = model.resolved_path("main");
    const auto file = utils::path_from_utf8(path);
    auto extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return std::tolower(value); });
    std::error_code error;
    if (extension != ".gguf" || !std::filesystem::is_regular_file(file, error)) return;
    const auto result = artifact("main", [&](const auto& target, auto& guard) {
        runtime::ArtifactServices reads(guard, path, false);
        return target.metadata({reads.root(), utils::path_to_utf8(utils::path_from_utf8(path).filename()), model.type, model.labels}, reads);
    });
    model.labels = result.labels;
    if (result.header) {
        const auto& header = *result.header;
        model.max_context_window = header.context_length;
        model.gguf = {header.architecture, header.context_length, header.block_count, header.embedding_length,
            header.head_count_kv, header.key_length, header.key_length_swa, header.swa_layer_count, header.full_attention_interval,
            header.capabilities, header.head_count_kv_per_layer, header.sliding_window_pattern, header.head_count_kv_scalar};
    }
}
std::string DefinitionRunner::validate_registration(const std::string& checkpoint) const {
    if (program_.artifacts.empty()) return "";
    return artifact("main", [&](const auto& target, auto&) { return target.validate_registration(checkpoint); });
}
std::string DefinitionRunner::validate_file(const std::string& path) const {
    if (program_.artifacts.empty()) return "";
    return artifact("main", [&](const auto& target, auto& guard) {
        const bool directory = std::filesystem::is_directory(utils::path_from_utf8(path));
        runtime::ArtifactServices reads(guard, path, directory);
        return target.validate_file({reads.root(), utils::path_to_utf8(utils::path_from_utf8(path).filename()), directory}, reads);
    });
}
std::string DefinitionRunner::resolve_artifact(const CheckpointResolveContext& context) const {
    if (program_.artifacts.empty()) throw std::logic_error("Configured v2 artifact consumer is unavailable");
    if (context.model_cache_path.empty()) return "";
    if (context.type == "npu_cache") return "";
    const auto role = context.type.empty() ? "main" : context.type;
    const bool declared = std::any_of(program_.artifacts.begin(), program_.artifacts.end(), [&](const auto& item) { return item.role == role; });
    const auto owner = declared ? role : "main";
    const auto resolved_role = !declared && role != "main" ? "mmproj" : role;
    return artifact(owner, [&](const auto& target, auto& guard) {
        runtime::ArtifactServices reads(guard, context.model_cache_path, true);
        return reads.approved_result(target.resolve_path({reads.root(), resolved_role, context.variant}, reads));
    });
}
std::string DefinitionRunner::find_imported(const std::string& path) const {
    if (program_.artifacts.empty()) return "";
    return artifact("main", [&](const auto& target, auto& guard) {
        runtime::ArtifactServices reads(guard, path, true);
        return reads.approved_result(target.find_imported({reads.root()}, reads));
    });
}

PreparedExecution DefinitionRunner::prepare_load(const LoadContext& context, const BackendSpec& spec,
    LaunchServices& services, utils::RequestCancelToken cancel, runtime::EnvironmentSources environment) const {
    if (spec.executable_definition.get() != executable_.get() || !valid_spec_aliases(*executable_, spec) || spec.definition ||
        spec.effective_public_recipe() != declaration()["recipe"] || spec.effective_pin_recipe() != spec.effective_public_recipe())
        throw std::logic_error("Install definition/spec ownership mismatch");
    runtime::Frame frame(*executable_);
    auto& values = frame.data.values;
    auto& options = frame.data.options;
    options = context.options.to_json();
    if (!options.contains("ctx_size")) options["ctx_size"] = context.options.get_option("ctx_size");
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
    if (records.value("load_log_policy", "none") == "loading_and_options") {
        const auto category = program_.normalized.at("metadata").at("log_category").get<std::string>();
        LOG(INFO, category.c_str()) << "Loading model: " << context.model_name << std::endl;
        LOG(DEBUG, category.c_str()) << "Per-model settings: " << context.options.to_log_string() << std::endl;
    }
    values = {{"options", options}, {"model", {{"name", context.model_name}, {"checkpoint", context.model_info.checkpoint()}}},
        {"platform", {{"os", host.os}, {"machine_arch", host.machine_arch}, {"rocm_arch", host.rocm_arch}}},
        {"artifacts", json::object()}, {"launch", {{"ports", json::object()}, {"log_level", context.log_level}}}};
    frame.facts = {typed::Fact::BackendOptions, typed::Fact::Platform, typed::Fact::VersionInputs,
        typed::Fact::ModelLabels, typed::Fact::DraftCheckpoint, typed::Fact::ModelArtifacts, typed::Fact::ContextSize};
    LaunchPlan plan;
    std::string backend;
    bool use_gpu = false;
    bool external_program = false;
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
                            const auto channel = backend == "rocm" ? services.rocm_channel() : "stable";
                            runtime::SelectionServices selection(guard, [&](const auto& requested) {
                                services.validate_backend(spec.effective_public_recipe(), requested);
                            });
                            return target.select_backend({backend, channel, {}}, selection);
                        });
                    backend = selected.backend; use_gpu = selected.use_gpu;
                }
            } else {
                if (backend.empty()) {
                    const auto supported = services.supported_backends(spec.effective_public_recipe());
                    if (supported.empty()) throw std::runtime_error("No supported configured backend");
                    backend = supported.front();
                }
                if (launch.backend_slot || records.value("device_assignment", "select_backend") != "preserve") services.validate_backend(spec.effective_public_recipe(), backend);
                use_gpu = backend != "cpu";
                frame.facts.insert(typed::Fact::SelectedBackend);
            }
            values["launch"]["backend"] = backend;
            if (records.value("device_assignment", "select_backend") == "select_backend" && context.device_selected)
                context.device_selected(use_gpu ? DEVICE_GPU : DEVICE_CPU);
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
            if (launch.install_policy == "always" || services.external_binary(spec, backend).empty()) services.install_backend(spec.effective_public_recipe(), backend);
        } else if (phase == "program") {
            if (launch.program_source == "external_or_managed" || program_.install.strategy == "external_only")
                plan.command.program = services.external_binary(spec, backend);
            external_program = !plan.command.program.empty();
            if (plan.command.program.empty()) {
                if (program_.install.strategy == "external_only") throw std::runtime_error("External backend executable is required");
                plan.command.program = services.managed_binary(spec, backend);
            }
            if (plan.command.program.empty() || (launch.program_source != "managed" && !services.exists(plan.command.program)))
                throw std::runtime_error("Configured backend executable is missing");
            values["launch"]["executable"] = plan.command.program;
            values["launch"]["executable_directory"] = utils::path_to_utf8(utils::path_from_utf8(plan.command.program).parent_path());
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
                if (number < 0 || (number == 0 && port.reject_zero) || number > 65535) throw std::runtime_error(port.error.empty() ? "Invalid configured port" : port.error);
                values["launch"]["ports"][port.name] = number;
            }
        }
        if (context.phase_prepared) context.phase_prepared(phase, values);
    }
    for (const auto index : slots(SlotOwnerKind::Root, "backend", "backend")) {
        invoke<typed::BackendSelectionOps>(frame, index, SlotOwnerKind::Root, "backend", "backend", cancel,
            [&](const auto& target, auto& guard) {
                runtime::SelectionServices selection(guard, {backend});
                const auto binding = std::find_if(program_.bindings.begin(), program_.bindings.end(),
                    [](const auto& item) { return item.id == "device"; });
                const auto device = binding == program_.bindings.end() ? std::string() : options.at(binding->public_name).template get<std::string>();
                const auto checked = target.validate_device({backend, device}, selection);
                if (!checked.supported) throw std::invalid_argument(checked.error);
                return true;
            });
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
    for (const auto index : slots(SlotOwnerKind::Root, "environment", "environment")) {
        plan.command.env = invoke<typed::EnvironmentHook>(frame, index, SlotOwnerKind::Root, "environment", "environment", cancel,
            [&](auto target, auto& guard) {
                std::optional<typed::StagingScope> scope;
                std::string destination;
                if (host.os == "windows" && backend == "rocm-stable" && (!external_program ||
                    executable_->compiled().authorized().definition().key.space == DefinitionNamespace::Builtin)) {
                    destination = services.staging_directory(plan.command.program);
                    scope = external_program ? typed::StagingScope::BuiltinExternalTheRock : typed::StagingScope::ManagedTheRock;
                }
                runtime::EnvironmentServices service(guard, environment, destination, scope);
                const auto device_binding = std::find_if(program_.bindings.begin(), program_.bindings.end(),
                    [](const auto& item) { return item.id == "device"; });
                const auto device = device_binding == program_.bindings.end() ? std::string() : options.at(device_binding->public_name).template get<std::string>();
                auto plan = target({runtime::typed_platform(host.os), backend, device,
                    values["launch"]["executable_directory"].get<std::string>(), service.capability()}, service);
                return runtime::StagingConsumer::execute(service.seal(std::move(plan)));
            });
    }
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
    if (launch.working_directory.at(host.os) == "executable_directory") plan.working_directory = utils::path_to_utf8(utils::path_from_utf8(plan.command.program).parent_path());
    const auto& levels = records["output"]["inherit_levels"];
    plan.output = {std::find(levels.begin(), levels.end(), context.log_level) != levels.end(), records["output"]["filter"].get<bool>()};
    runtime::check_cancelled(cancel);
    auto session = std::make_shared<const ExecutionSession>(ExecutionSession{executable_, frame.data, frame.facts, frame.produced, context.model_name});
    return {std::move(plan), std::move(session)};
}

}
