#include "lemon/backends/configured/backend_hook_registry.h"

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
        if (!registration.parameter_schema.is_object() || registration.parameter_schema.value("type", "") != "object" ||
            !registration.parameter_schema.contains("additionalProperties") || registration.parameter_schema["additionalProperties"] != false) {
            throw std::invalid_argument("Hook parameter schema must be closed");
        }
        for (const auto& operation : registration.operations) {
            const bool json_stage = registration.stage == HookStage::JsonOperation && operation.delivery == OperationDelivery::Buffered;
            const bool sink_stage = registration.stage == HookStage::SinkOperation && operation.delivery != OperationDelivery::Realtime;
            if (!(json_stage || sink_stage) || !valid_operation_delivery(operation.operation, operation.delivery) ||
                !operation.model_types || (operation.model_types & ~k_all_operation_model_types) || operation.provenance != OperationProvenance::Implemented) {
                throw std::invalid_argument("Invalid hook operation signature");
            }
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

}
