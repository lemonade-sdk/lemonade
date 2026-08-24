#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/smart_thinking_search.h"

namespace lemon {

enum class SmartThinkingContractDetectionStatus {
    NoMatch,
    Eligible,
    Rejected
};

inline const char* to_string(SmartThinkingContractDetectionStatus status) {
    switch (status) {
        case SmartThinkingContractDetectionStatus::NoMatch: return "no_match";
        case SmartThinkingContractDetectionStatus::Eligible: return "eligible";
        case SmartThinkingContractDetectionStatus::Rejected: return "rejected";
    }
    return "unknown";
}

struct SmartThinkingCapabilityLimits {
    std::size_t max_input_bytes = 0;
    std::size_t max_items = 0;
    std::size_t max_edges = 0;
    std::size_t max_identifier_bytes = 0;
    std::int64_t max_numeric_magnitude = 0;
    std::int64_t max_execution_ms = 0;
};

struct SmartThinkingCapabilityDescriptor {
    std::string family_id;
    std::string contract_version;
    std::string detector_name;
    std::string parser_name;
    std::string semantic_validator_name;
    std::string executor_name;
    std::string serializer_name;
    SmartThinkingCapabilityLimits limits;
};

struct SmartThinkingContractDetectionResult {
    SmartThinkingContractDetectionStatus status =
        SmartThinkingContractDetectionStatus::NoMatch;
    std::string family_id;
    std::string contract_version;
    std::string reason;
    std::vector<std::string> matched_markers;
    std::vector<std::string> missing_markers;

    bool matched() const {
        return status != SmartThinkingContractDetectionStatus::NoMatch;
    }

    bool eligible() const {
        return status == SmartThinkingContractDetectionStatus::Eligible;
    }

    nlohmann::json to_json() const {
        return {
            {"status", to_string(status)},
            {"family_id", family_id},
            {"contract_version", contract_version},
            {"reason", reason},
            {"matched_markers", matched_markers},
            {"missing_markers", missing_markers}
        };
    }
};


class ISmartThinkingCompiledCapabilityTask {
public:
    virtual ~ISmartThinkingCompiledCapabilityTask() = default;
    virtual const std::string& family_id() const = 0;
    virtual const std::string& contract_version() const = 0;
};

struct SmartThinkingCapabilityCompileResult {
    SmartThinkingCompileStatus status = SmartThinkingCompileStatus::NoMatch;
    std::shared_ptr<const ISmartThinkingCompiledCapabilityTask> typed_task;
    nlohmann::json compiled_task = nlohmann::json::object();
    std::string failure_reason;
    std::string program_hash;
    int operation_count = 0;
    int task_count = 0;

    bool compiled() const {
        return status == SmartThinkingCompileStatus::Compiled && typed_task != nullptr;
    }
};

struct SmartThinkingCapabilityExecutionResult {
    bool completed = false;
    std::string final_text;
    std::string stop_reason;
    std::string verifier_summary;
    std::string failure_reason;
    int chunk_size = 0;
    int proposal_calls = 0;
    int model_calls = 0;
    int transition_attempts = 0;
    int operations_executed = 0;
    int completion_events = 0;
    int tasks_completed = 0;
    int search_nodes = 0;
    int verification_claims = 0;
    int verification_checks = 0;
    int verification_events = 0;
    nlohmann::json verification_ledger = nlohmann::json::object();
    std::int64_t execution_time_ms = 0;
};

class ISmartThinkingVerifiedCapability {
public:
    virtual ~ISmartThinkingVerifiedCapability() = default;

    virtual SmartThinkingCapabilityDescriptor descriptor() const = 0;
    virtual SmartThinkingContractDetectionResult detect(
        const std::string& task_text) const = 0;
    virtual SmartThinkingCapabilityCompileResult compile(
        const std::string& task_text) const = 0;
    virtual SmartThinkingCapabilityExecutionResult execute(
        const SmartThinkingCapabilityCompileResult& compiled,
        SmartThinkingSearchEventLog* event_log) const = 0;
};

struct SmartThinkingCapabilityRouteResult {
    std::shared_ptr<const ISmartThinkingVerifiedCapability> capability;
    SmartThinkingContractDetectionResult detection;
    bool ambiguous = false;
    std::vector<std::string> matched_families;

    bool matched() const {
        return detection.matched();
    }

    bool eligible() const {
        return capability != nullptr && detection.eligible() && !ambiguous;
    }
};

class SmartThinkingCapabilityRegistry {
public:
    bool register_capability(
        std::shared_ptr<const ISmartThinkingVerifiedCapability> capability) {
        if (capability == nullptr) return false;
        const auto descriptor = capability->descriptor();
        if (descriptor.family_id.empty() || descriptor.contract_version.empty()) {
            return false;
        }
        for (const auto& existing : capabilities_) {
            const auto existing_descriptor = existing->descriptor();
            if (existing_descriptor.family_id == descriptor.family_id &&
                existing_descriptor.contract_version == descriptor.contract_version) {
                return false;
            }
        }
        capabilities_.push_back(std::move(capability));
        return true;
    }

    SmartThinkingCapabilityRouteResult route(
        const std::string& task_text) const {
        SmartThinkingCapabilityRouteResult result;
        for (const auto& capability : capabilities_) {
            const auto detection = capability->detect(task_text);
            if (!detection.matched()) continue;
            result.matched_families.push_back(detection.family_id);
            if (result.detection.status ==
                SmartThinkingContractDetectionStatus::NoMatch) {
                result.detection = detection;
                result.capability = capability;
            } else {
                result.ambiguous = true;
                result.capability.reset();
                result.detection.status =
                    SmartThinkingContractDetectionStatus::Rejected;
                result.detection.family_id = "ambiguous";
                result.detection.contract_version.clear();
                result.detection.reason = "multiple_verified_contracts_matched";
            }
        }
        return result;
    }

    std::vector<SmartThinkingCapabilityDescriptor> descriptors() const {
        std::vector<SmartThinkingCapabilityDescriptor> result;
        result.reserve(capabilities_.size());
        for (const auto& capability : capabilities_) {
            result.push_back(capability->descriptor());
        }
        return result;
    }

    std::size_t size() const {
        return capabilities_.size();
    }

private:
    std::vector<std::shared_ptr<const ISmartThinkingVerifiedCapability>> capabilities_;
};

SmartThinkingCapabilityRegistry make_default_smart_thinking_capability_registry();

}  // namespace lemon
