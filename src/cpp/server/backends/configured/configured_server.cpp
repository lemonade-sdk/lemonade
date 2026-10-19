#include "lemon/backends/configured/configured_server.h"
#include "lemon/utils/aixlog.hpp"

namespace lemon::backends {
namespace {
std::string effective_level(const ConfiguredBackendEntry& entry, const std::string& level) {
    return entry.definition->records["metadata"]["log_level_policy"] == "debug_flag" && level != "debug" ? "info" : level;
}
std::string_view handler(BackendOperation operation, OperationDelivery delivery) {
    if (operation == BackendOperation::AudioGeneration && delivery == OperationDelivery::Buffered) return "audio";
    if (operation == BackendOperation::ChatCompletion) return delivery == OperationDelivery::Buffered ? "chat.buffered" : "chat.streaming";
    if (operation == BackendOperation::Completion) return delivery == OperationDelivery::Buffered ? "completion.buffered" : "completion.streaming";
    if (operation == BackendOperation::Responses) return delivery == OperationDelivery::Buffered ? "responses.buffered" : "responses.streaming";
    throw std::invalid_argument("Configured operation adapter is not installed");
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
        const auto id = handler(description.operation, description.delivery);
        const bool buffered = description.delivery == OperationDelivery::Buffered;
        const bool audio = description.operation == BackendOperation::AudioGeneration;
        const bool executable = audio ? program.kind == "http_bytes" && program.codec == "raw_bytes.v1" :
            buffered ? program.kind == "http_json" && program.codec == "json.v1" : program.kind == "http_sse" && program.codec == "openai_sse.v1";
        if (!executable) throw std::invalid_argument("Configured transport signature is not installed");
        bindings.push_back({description, id, audio ? "media" : buffered ? "json" : "sse"});
    }
    const bool compatibility = trusted == TrustedCompatibility::LegacyNonChatSse;
    if (compatibility) {
        std::vector<OperationBinding> legacy;
        for (const auto& [operation, endpoint] : endpoints) legacy.push_back({{operation, OperationDelivery::Streaming, k_all_operation_model_types, OperationProvenance::LegacyForwarding}, handler(operation, OperationDelivery::Streaming), "sse"});
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
          [this](const auto& endpoint, const auto& body, auto& sink, bool sse, long timeout, TelemetryCallback callback) {
              WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout, std::move(callback));
          }}) {
    if (!services_.launch) services_.launch = std::make_shared<EffectLaunchServices>(production_backend_effects(context.backend_manager, [this] { return choose_port(); }), entry_->launch_compatibility.posix_executable_loader_path);
    if (!services_.make_process) services_.make_process = [](ProcessOutput output, const std::string& cwd) { return std::make_unique<NativeProcess>(output, cwd); };
    OperationAdapters adapters;
    adapters.completion = this;
    adapters.audio_generation = this;
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

void ConfiguredServer::load(const std::string& name, const ModelInfo& model, const RecipeOptions& options, bool do_not_upgrade) {
    const std::string category = entry_->definition->records["metadata"]["log_category"];
    const auto& log = entry_->launch_compatibility.load_log;
    if (!log) LOG(INFO, category.c_str()) << "Loading model: " << name << std::endl;
    else if (log->loading) log->loading(category, name);
    LaunchPlanner planner;
    LoadContext context{name, model, options, log_level_, do_not_upgrade, {}};
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
    start_server(services_.make_process(plan.output, plan.working_directory), plan.command, plan.readiness_timeout_seconds);
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
json ConfiguredServer::send_completion(BackendOperation operation, const json& request) {
    if (const auto* selected = program(operation, OperationDelivery::Buffered)) return transport_.send_json(RequestMapper().map(*selected, mapping_, request));
    if (operation == BackendOperation::ChatCompletion) return WrappedServer::chat_completion(request);
    if (operation == BackendOperation::Completion) return WrappedServer::completion(request);
    return WrappedServer::responses(request);
}
json ConfiguredServer::chat_completion(const json& request) { return send_completion(BackendOperation::ChatCompletion, request); }
json ConfiguredServer::completion(const json& request) { return send_completion(BackendOperation::Completion, request); }
json ConfiguredServer::responses(const json& request) { return send_completion(BackendOperation::Responses, request); }
void ConfiguredServer::audio_generations(const json& request, httplib::DataSink& sink) {
    const auto* selected = program(BackendOperation::AudioGeneration, OperationDelivery::Buffered);
    if (!selected) throw std::invalid_argument("Audio generation adapter is not bound");
    transport_.send_stream(RequestMapper().map(*selected, mapping_, request), sink);
}
std::vector<std::string> ConfiguredServer::supported_audio_formats() const {
    const auto* selected = program(BackendOperation::AudioGeneration, OperationDelivery::Buffered);
    return selected ? selected->formats : std::vector<std::string>{};
}
AudioFormatMetadata ConfiguredServer::audio_format_metadata(const std::string& format) const {
    AudioFormatMetadata metadata;
    const auto* selected = program(BackendOperation::AudioGeneration, OperationDelivery::Buffered);
    if (selected) {
        const auto found = selected->format_metadata.find(format);
        if (found != selected->format_metadata.end()) {
            metadata.content_type = found->second.content_type;
            metadata.headers = found->second.headers;
        }
    }
    return metadata;
}
void ConfiguredServer::forward_streaming_request(const std::string& endpoint, const std::string& body, httplib::DataSink& sink, bool sse, long timeout, TelemetryCallback callback) {
    for (const auto& [operation, path] : endpoints) {
        if (path != endpoint) continue;
        if (const auto* selected = program(operation, OperationDelivery::Streaming)) {
            if (selected->identity && selected->remove.empty() && selected->set.empty()) {
                WrappedServer::forward_streaming_request(selected->path, body, sink, sse,
                    selected->timeout_seconds == 0 ? timeout : selected->timeout_seconds, std::move(callback));
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
