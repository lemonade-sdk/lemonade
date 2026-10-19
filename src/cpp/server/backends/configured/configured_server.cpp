#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/utils/aixlog.hpp"

namespace lemon::backends {
namespace {
std::string effective_level(const ConfiguredBackendEntry& entry, const std::string& level) {
    return entry.definition->records["metadata"]["log_level_policy"] == "debug_flag" && level != "debug" ? "info" : level;
}
OperationHandlerIdentity identity(BackendOperation operation, OperationDelivery delivery) {
    OperationHandlerIdentity result;
    if (!operation_handler_identity(operation, delivery, result)) throw std::invalid_argument("Configured operation adapter is not installed");
    return result;
}
bool executable(const OperationProgram& program) {
    const auto operation = program.description.operation;
    if (!program.handler.empty()) return program.kind.empty();
    if (operation == BackendOperation::AudioGeneration || operation == BackendOperation::TextToSpeech) return program.kind == "http_bytes" && program.codec == "raw_bytes.v1";
    if (operation == BackendOperation::StreamingTranscription) return program.kind == "realtime" && program.codec == "jsonl.v1" && program.method == "TCP";
    if (program.description.delivery != OperationDelivery::Buffered) return program.kind == "http_sse" && program.codec == "openai_sse.v1";
    return program.kind == "http_json" && program.codec == "json.v1" && program.method == (operation == BackendOperation::Slots ? "GET" : "POST");
}
const std::pair<BackendOperation, std::string_view> endpoints[] = {
    {BackendOperation::ChatCompletion, "/v1/chat/completions"},
    {BackendOperation::Completion, "/v1/completions"},
    {BackendOperation::Responses, "/v1/responses"}
};
}

std::vector<OperationBinding> configured_operation_bindings(const BackendDefinition& definition, TrustedCompatibility trusted) {
    std::vector<OperationBinding> bindings;
    for (const auto& program : definition.operations) {
        const auto& description = program.description;
        if (!executable(program)) throw std::invalid_argument("Configured transport signature is not installed");
        const auto handler = identity(description.operation, description.delivery);
        bindings.push_back({description, handler.handler, handler.codec});
    }
    const bool compatibility = trusted == TrustedCompatibility::LegacyNonChatSse;
    if (compatibility) {
        std::vector<OperationBinding> legacy;
        for (const auto& [operation, endpoint] : endpoints) legacy.push_back({{operation, OperationDelivery::Streaming, k_all_operation_model_types, OperationProvenance::LegacyForwarding}, identity(operation, OperationDelivery::Streaming).handler, "sse"});
        bindings.insert(bindings.begin(), legacy.begin(), legacy.end());
    }
    validate_operation_binding_signatures(bindings, compatibility);
    return bindings;
}

ConfiguredServer::ConfiguredServer(std::shared_ptr<const ConfiguredBackendEntry> entry, const BackendContext& context, ConfiguredServerServices services)
    : WrappedServer(entry->definition->records["metadata"]["server_name"], effective_level(*entry, context.log_level), context.model_manager, context.backend_manager),
      entry_(std::move(entry)), services_(std::move(services)),
      transport_(TransportAccess{
          [this](const auto& endpoint, const auto& body, long timeout) { return WrappedServer::forward_request(endpoint, body, timeout); },
          [this](const auto& endpoint, long timeout) { return WrappedServer::forward_get_request(endpoint, timeout); },
          [this](const auto& endpoint, const auto& body, auto& sink, bool sse, long timeout, TelemetryCallback callback) {
              WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout, std::move(callback));
          }}) {
    if (!services_.launch) services_.launch = std::make_shared<EffectLaunchServices>(production_backend_effects(context.backend_manager, [this] { return choose_port(); }), entry_->launch_compatibility.posix_executable_loader_path);
    if (!services_.make_process) services_.make_process = [](ProcessOutput output, const std::string& cwd) { return std::make_unique<NativeProcess>(output, cwd); };
    OperationAdapters adapters;
    adapters.completion = this;
    adapters.embeddings = this;
    adapters.reranking = this;
    adapters.classification = this;
    adapters.transcription = this;
    adapters.streaming_transcription = this;
    adapters.slots = this;
    adapters.tokenizer = this;
    adapters.text_to_speech = this;
    adapters.audio_generation = this;
    adapters.model_3d = this;
    adapters.completion_rejections = {
        [](WrappedServer& server, const json& request) { return server.chat_completion(request); },
        [](WrappedServer& server, const json& request) { return server.completion(request); },
        [](WrappedServer& server, const json& request) { return server.responses(request); }
    };
    adapters.responses = [](WrappedServer& server, const json& request) { return server.responses(request); };
    adapters.streaming_forwarder = [](WrappedServer& server, const std::string& endpoint, const std::string& body, httplib::DataSink& sink, bool sse, long timeout, TelemetryCallback callback) {
        server.forward_streaming_request(endpoint, body, sink, sse, timeout, std::move(callback));
    };
    for (const auto& program : entry_->definition->operations) {
        if (program.description.operation <= BackendOperation::Responses) adapters.completion_implementations[static_cast<size_t>(program.description.operation)] = true;
    }
    const bool compatibility = entry_->compatibility == TrustedCompatibility::LegacyNonChatSse;
    bind_operations(OperationBindings(adapters, entry_->bindings, compatibility));
    set_descriptor(&entry_->descriptor);
}

void ConfiguredServer::load(const std::string& name, const ModelInfo& model, const RecipeOptions& options, bool) {
    const std::string category = entry_->definition->records["metadata"]["log_category"];
    const auto& log = entry_->launch_compatibility.load_log;
    LaunchPlanner planner;
    planner.require_platform(*entry_->definition, *services_.launch);
    if (!log) LOG(INFO, category.c_str()) << "Loading model: " << name << std::endl;
    else if (log->loading) log->loading(category, name, options);
    LoadContext context{name, model, options, log_level_, {}};
    if (entry_->launch_compatibility.device_on_backend_selection) context.backend_selected = [this] {
        device_type_ = *entry_->launch_compatibility.device_on_backend_selection;
    };
    if (log && log->prepared) context.phase_prepared = [&](const std::string& phase, const json& values) { log->prepared(category, phase, values); };
    const auto inputs = planner.prepare(*entry_->definition, entry_->spec, context, *services_.launch);
    mapping_.values = inputs.values;
    const auto plan = planner.plan(*entry_->definition, inputs);
    configure_backend_watchdog({plan.command.ready_endpoint, plan.watchdog_enabled, true});
    if (!log) LOG(INFO, category.c_str()) << "Starting " << plan.command.program << " on port " << plan.command.port << std::endl;
    else if (log->starting) log->starting(category, inputs.values, plan.command);
    const auto hooks = entry_->definition->records.value("hooks", json::object());
    try {
        start_server(services_.make_process(plan.output, plan.working_directory), plan.command, plan.readiness_timeout_seconds);
    } catch (const std::runtime_error& error) {
        if (!hooks.contains("start_failure") || load_cancelled()) throw;
        const auto details = installed_backend_hooks().target<StartFailureHook>(hooks["start_failure"]["id"].get<std::string>(), HookStage::StartFailure)(plan.command.program, plan.command.args);
        throw std::runtime_error(std::string(error.what()) + (details.empty() ? "" : ": " + details));
    }
}
const OperationProgram* ConfiguredServer::program(BackendOperation operation, OperationDelivery delivery) const {
    const OperationProgram* fallback = nullptr;
    size_t matches = 0;
    for (const auto& program : entry_->definition->operations) {
        if (program.description.operation != operation || program.description.delivery != delivery) continue;
        ++matches;
        if (!fallback) fallback = &program;
        if (program.description.model_types & operation_model_type_mask(get_model_type())) return &program;
    }
    return matches == 1 ? fallback : nullptr;
}
const OperationProgram& ConfiguredServer::bound(BackendOperation operation) const {
    const auto* selected = program(operation, OperationDelivery::Buffered);
    if (!selected) throw std::invalid_argument(std::string(backend_operation_name(operation)) + " adapter is not bound");
    return *selected;
}
json ConfiguredServer::send_json(const MappedRequest& mapped, const json& request) {
    auto response = transport_.send_json(mapped);
    for (const auto& id : mapped.operation->response_hooks) response = installed_backend_hooks().target<ResponseHook>(id, HookStage::Response)(std::move(response), request, get_model_name());
    return response;
}
json ConfiguredServer::send_completion(BackendOperation operation, const json& request) {
    if (const auto* selected = program(operation, OperationDelivery::Buffered)) return send_json(RequestMapper().map(*selected, mapping_, request), request);
    const auto messages = entry_->definition->records.value("unsupported_messages", json::object());
    const std::string name(backend_operation_name(operation));
    if (messages.contains(name)) return json{{"error", {{"message", messages[name]}, {"type", "unsupported_operation"}, {"code", "model_not_applicable"}}}};
    if (operation == BackendOperation::ChatCompletion) return WrappedServer::chat_completion(request);
    if (operation == BackendOperation::Completion) return WrappedServer::completion(request);
    return WrappedServer::responses(request);
}
json ConfiguredServer::chat_completion(const json& request) { return send_completion(BackendOperation::ChatCompletion, request); }
json ConfiguredServer::completion(const json& request) { return send_completion(BackendOperation::Completion, request); }
json ConfiguredServer::responses(const json& request) { return send_completion(BackendOperation::Responses, request); }
json ConfiguredServer::embeddings(const json& request) { return send_json(RequestMapper().map(bound(BackendOperation::Embeddings), mapping_, request), request); }
json ConfiguredServer::reranking(const json& request) { return send_json(RequestMapper().map(bound(BackendOperation::Reranking), mapping_, request), request); }
json ConfiguredServer::get_slots() { return send_json(RequestMapper().map(bound(BackendOperation::Slots), mapping_, json::object()), json::object()); }
json ConfiguredServer::slots_action(int slot_id, const std::string& action, const json& request_body) {
    auto mapped = RequestMapper().map(bound(BackendOperation::SlotAction), mapping_, request_body);
    for (const auto& [name, value] : {std::pair<std::string, std::string>{"{slot_id}", std::to_string(slot_id)}, {"{action}", action}}) {
        const auto found = mapped.path.find(name);
        if (found != std::string::npos) mapped.path.replace(found, name.size(), value);
    }
    return send_json(mapped, request_body);
}
json ConfiguredServer::tokenize(const json& request_body) { return send_json(RequestMapper().map(bound(BackendOperation::Tokenize), mapping_, request_body), request_body); }
OperationHookContext ConfiguredServer::hook_context() {
    return {mapping_.values, entry_->definition->records["metadata"]["log_category"].get_ref<const std::string&>(), get_backend_port(), get_base_url(),
        [this](const std::string& endpoint, const json& body, long timeout) { return WrappedServer::forward_request(endpoint, body, timeout); }};
}
json ConfiguredServer::classify(const json& request) {
    const auto& selected = bound(BackendOperation::Classification);
    return installed_backend_hooks().target<OperationHook>(selected.handler, HookStage::Operation)(hook_context(), request);
}
json ConfiguredServer::audio_transcriptions(const json& request) {
    const auto& selected = bound(BackendOperation::Transcription);
    return installed_backend_hooks().target<OperationHook>(selected.handler, HookStage::Operation)(hook_context(), request);
}
std::string ConfiguredServer::get_streaming_address() {
    const auto* selected = program(BackendOperation::StreamingTranscription, OperationDelivery::Realtime);
    if (!selected) throw std::invalid_argument("Streaming transcription adapter is not bound");
    const json::json_pointer port("/launch/ports/" + selected->port);
    return mapping_.values.contains(port) ? "tcp://127.0.0.1:" + std::to_string(mapping_.values.at(port).get<int>()) : "";
}
const OperationProgram* ConfiguredServer::media(OperationDelivery delivery) const {
    if (const auto* speech = program(BackendOperation::TextToSpeech, delivery)) return speech;
    return delivery == OperationDelivery::Buffered ? program(BackendOperation::AudioGeneration, delivery) : nullptr;
}
void ConfiguredServer::audio_speech(const json& request, httplib::DataSink& sink) {
    const auto* selected = program(BackendOperation::TextToSpeech, OperationDelivery::Buffered);
    if (!selected) selected = program(BackendOperation::TextToSpeech, OperationDelivery::Streaming);
    if (!selected) throw std::invalid_argument("Text to speech adapter is not bound");
    transport_.send_stream(RequestMapper().map(*selected, mapping_, request), sink);
}
void ConfiguredServer::audio_generations(const json& request, httplib::DataSink& sink) {
    const auto* selected = program(BackendOperation::AudioGeneration, OperationDelivery::Buffered);
    if (!selected) throw std::invalid_argument("Audio generation adapter is not bound");
    transport_.send_stream(RequestMapper().map(*selected, mapping_, request), sink);
}
void ConfiguredServer::model_3d_generations(const json& request, httplib::DataSink& sink) {
    const auto& selected = bound(BackendOperation::Model3DGeneration);
    installed_backend_hooks().target<SinkOperationHook>(selected.handler, HookStage::SinkOperation)(hook_context(), request, sink);
}
std::vector<std::string> ConfiguredServer::supported_audio_formats() const {
    const auto* selected = media(OperationDelivery::Buffered);
    return selected ? selected->formats : std::vector<std::string>{};
}
std::vector<std::string> ConfiguredServer::supported_streaming_audio_formats() const {
    const auto* selected = media(OperationDelivery::Streaming);
    return selected ? selected->formats : std::vector<std::string>{};
}
AudioFormatMetadata ConfiguredServer::audio_format_metadata(const std::string& format) const {
    for (const auto delivery : {OperationDelivery::Buffered, OperationDelivery::Streaming}) {
        const auto* selected = media(delivery);
        if (!selected) continue;
        const auto found = selected->format_metadata.find(format);
        if (found != selected->format_metadata.end()) return {found->second.content_type, found->second.headers};
    }
    return {};
}
void ConfiguredServer::forward_streaming_request(const std::string& endpoint, const std::string& body, httplib::DataSink& sink, bool sse, long timeout, TelemetryCallback callback) {
    for (const auto& [operation, path] : endpoints) {
        if (path != endpoint) continue;
        if (const auto* selected = program(operation, OperationDelivery::Streaming)) {
            const long effective_timeout = selected->timeout_seconds == 0 ? timeout : selected->timeout_seconds;
            if (selected->identity && selected->remove.empty() && selected->set.empty() && selected->request_hooks.empty()) {
                WrappedServer::forward_streaming_request(selected->path, body, sink, sse, effective_timeout, std::move(callback));
                return;
            }
            if (!selected->request_hooks.empty()) {
                const auto parsed = json::parse(body, nullptr, false);
                const auto forwarded = parsed.is_discarded() ? body : RequestMapper().map(*selected, mapping_, parsed).body.dump();
                WrappedServer::forward_streaming_request(selected->path, forwarded, sink, sse, effective_timeout, std::move(callback));
                return;
            }
            const auto request = json::parse(body);
            transport_.send_stream(RequestMapper().map(*selected, mapping_, request, body), sink, std::move(callback));
            return;
        }
        if (entry_->compatibility == TrustedCompatibility::LegacyNonChatSse) {
            WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout, std::move(callback));
            return;
        }
    }
    if (entry_->compatibility == TrustedCompatibility::LegacyNonChatSse) {
        WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout, std::move(callback));
        return;
    }
    throw std::invalid_argument("Streaming operation adapter is not bound");
}

}
