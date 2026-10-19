#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/kokoro/kokoro_hooks.h"
#include "lemon/backends/llamacpp/llamacpp_request.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/moonshine/moonshine_hooks.h"

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

std::string_view hook_stage_name(HookStage stage) {
    switch (stage) {
        case HookStage::Artifacts: return "artifacts";
        case HookStage::Request: return "request";
        case HookStage::Response: return "response";
        case HookStage::Launch: return "launch";
        case HookStage::Operation: return "operation";
    }
    return "";
}

const BackendHookRegistry& installed_backend_hooks() {
    static const BackendHookRegistry registry({
        {"llamacpp.gguf", HookStage::Artifacts, ArtifactHook(&llamacpp::ops)},
        {"llamacpp.max_tokens_alias", HookStage::Request, RequestHook(&llamacpp::with_max_tokens_alias)},
        {"llamacpp.tool_schema_limits", HookStage::Request, RequestHook(&llamacpp::sanitize_tool_schema_limits)},
        {"llamacpp.client_model", HookStage::Response, ResponseHook(&llamacpp::normalize_response_model)},
        {"kokoro.voice_index", HookStage::Launch, LaunchHook(&kokoro::voice_index)},
        {"moonshine.model_arch", HookStage::Launch, LaunchHook(&moonshine::model_arch)},
        {"moonshine.transcription", HookStage::Operation, OperationHook(&moonshine::transcription)}
    });
    return registry;
}

}
