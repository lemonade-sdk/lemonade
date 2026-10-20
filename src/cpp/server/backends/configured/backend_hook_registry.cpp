#include "lemon/backends/configured/backend_definition_exports.h"
#include "lemon/backends/configured/backend_hook_abi.h"
#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/kokoro/kokoro_hooks.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/moonshine/moonshine_hooks.h"
#include "lemon/backends/onnxruntime/onnxruntime_hooks.h"
#include "lemon/backends/trellis/trellis_hooks.h"

#include <set>

namespace lemon::backends {

BackendHookRegistry::BackendHookRegistry(std::vector<HookRegistration> registrations)
    : registrations_(std::move(registrations)) {
    std::set<std::string> ids;
    for (const auto& registration : registrations_) {
        if (registration.id.empty() || !ids.insert(registration.id).second ||
            !std::visit([](auto function) { return function != nullptr; }, registration.target)) {
            throw std::invalid_argument("Duplicate, empty, or null hook registration");
        }
        if (static_cast<size_t>(registration.stage) != registration.target.index()) {
            throw std::invalid_argument("Hook stage/signature mismatch");
        }
    }
}

const HookRegistration& BackendHookRegistry::require(std::string_view id, HookStage expected_stage) const {
    for (const auto& registration : registrations_) {
        if (registration.id == id) {
            if (registration.stage != expected_stage) throw std::invalid_argument("Hook stage mismatch");
            return registration;
        }
    }
    throw std::invalid_argument("Unknown hook");
}

void audit_hook_abi_metadata(const BackendHookRegistry& registry) {
    static const char* stages[] = {"artifacts", "request", "response", "launch", "operation", "start_failure", "sink_operation"};
    if (registry.registrations().size() != installed_hook_abis().size()) throw std::logic_error("Hook ABI registry size mismatch");
    for (const auto& registration : registry.registrations()) {
        const auto* abi = find_hook_abi(registration.id);
        const auto stage = static_cast<std::size_t>(registration.stage);
        if (!abi || stage >= sizeof(stages) / sizeof(stages[0]) || abi->stage != stages[stage] || !abi->revision || !abi->implementation_revision)
            throw std::logic_error("Hook ABI registry mismatch");
    }
}

const BackendHookRegistry& installed_backend_hooks() {
    static const BackendHookRegistry registry = [] {
        BackendHookRegistry value({
        {"llamacpp.gguf", HookStage::Artifacts, ArtifactHook(&llamacpp::ops)},
        {"llamacpp.max_tokens_alias", HookStage::Request, RequestHook(&llamacpp::with_max_tokens_alias)},
        {"llamacpp.tool_schema_limits", HookStage::Request, RequestHook(&llamacpp::sanitize_tool_schema_limits)},
        {"llamacpp.client_model", HookStage::Response, ResponseHook(&llamacpp::normalize_response_model)},
        {"kokoro.voice_index", HookStage::Launch, LaunchHook(&kokoro::voice_index)},
        {"moonshine.model_arch", HookStage::Launch, LaunchHook(&moonshine::model_arch)},
        {"moonshine.transcription", HookStage::Operation, OperationHook(&moonshine::transcription)},
        {"onnxruntime.model_directory", HookStage::Artifacts, ArtifactHook(&onnxruntime::model_directory_ops)},
        {"onnxruntime.servable_directory", HookStage::Launch, LaunchHook(&onnxruntime::servable_directory)},
        {"onnxruntime.classify", HookStage::Operation, OperationHook(&onnxruntime::classify)},
        {"onnxruntime.startup_error", HookStage::StartFailure, StartFailureHook(&onnxruntime::startup_error)},
        {"trellis.mesh", HookStage::SinkOperation, SinkOperationHook(&trellis::mesh)}
        });
        audit_hook_abi_metadata(value);
        installed_builtin_exports();
        return value;
    }();
    return registry;
}

}
