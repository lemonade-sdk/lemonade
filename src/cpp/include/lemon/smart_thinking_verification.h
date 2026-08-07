#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

enum class SmartThinkingVerificationTier {
    None = 0,
    Structural = 1,
    Evidence = 2,
    Observable = 3,
    Deterministic = 4
};

enum class SmartThinkingClaimStatus {
    Unknown,
    Verified,
    Refuted,
    Unresolved
};

enum class SmartThinkingActionStatus {
    Pending,
    Running,
    Executed,
    Rejected
};

enum class SmartThinkingCheckStatus {
    Pending,
    Passed,
    Failed,
    Skipped
};

enum class SmartThinkingVerificationFailurePolicy {
    Abort,
    MarkUnresolved,
    NativeFallback
};

enum class SmartThinkingVerificationEventType {
    PlanValidated,
    ActionStarted,
    ObservationRecorded,
    ActionRejected,
    CheckPassed,
    CheckFailed,
    ClaimVerified,
    ClaimRefuted,
    ClaimUnresolved,
    PlanCompleted,
    PlanAborted
};

inline const char* to_string(SmartThinkingVerificationTier tier) {
    switch (tier) {
        case SmartThinkingVerificationTier::None: return "none";
        case SmartThinkingVerificationTier::Structural: return "structural";
        case SmartThinkingVerificationTier::Evidence: return "evidence";
        case SmartThinkingVerificationTier::Observable: return "observable";
        case SmartThinkingVerificationTier::Deterministic: return "deterministic";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingClaimStatus status) {
    switch (status) {
        case SmartThinkingClaimStatus::Unknown: return "unknown";
        case SmartThinkingClaimStatus::Verified: return "verified";
        case SmartThinkingClaimStatus::Refuted: return "refuted";
        case SmartThinkingClaimStatus::Unresolved: return "unresolved";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingActionStatus status) {
    switch (status) {
        case SmartThinkingActionStatus::Pending: return "pending";
        case SmartThinkingActionStatus::Running: return "running";
        case SmartThinkingActionStatus::Executed: return "executed";
        case SmartThinkingActionStatus::Rejected: return "rejected";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingCheckStatus status) {
    switch (status) {
        case SmartThinkingCheckStatus::Pending: return "pending";
        case SmartThinkingCheckStatus::Passed: return "passed";
        case SmartThinkingCheckStatus::Failed: return "failed";
        case SmartThinkingCheckStatus::Skipped: return "skipped";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingVerificationFailurePolicy policy) {
    switch (policy) {
        case SmartThinkingVerificationFailurePolicy::Abort: return "abort";
        case SmartThinkingVerificationFailurePolicy::MarkUnresolved:
            return "mark_unresolved";
        case SmartThinkingVerificationFailurePolicy::NativeFallback:
            return "native_fallback";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingVerificationEventType type) {
    switch (type) {
        case SmartThinkingVerificationEventType::PlanValidated:
            return "plan_validated";
        case SmartThinkingVerificationEventType::ActionStarted:
            return "action_started";
        case SmartThinkingVerificationEventType::ObservationRecorded:
            return "observation_recorded";
        case SmartThinkingVerificationEventType::ActionRejected:
            return "action_rejected";
        case SmartThinkingVerificationEventType::CheckPassed:
            return "check_passed";
        case SmartThinkingVerificationEventType::CheckFailed:
            return "check_failed";
        case SmartThinkingVerificationEventType::ClaimVerified:
            return "claim_verified";
        case SmartThinkingVerificationEventType::ClaimRefuted:
            return "claim_refuted";
        case SmartThinkingVerificationEventType::ClaimUnresolved:
            return "claim_unresolved";
        case SmartThinkingVerificationEventType::PlanCompleted:
            return "plan_completed";
        case SmartThinkingVerificationEventType::PlanAborted:
            return "plan_aborted";
    }
    return "unknown";
}

struct SmartThinkingVerificationClaim {
    std::string id;
    std::string proposition;
    SmartThinkingVerificationTier required_tier =
        SmartThinkingVerificationTier::Structural;
    SmartThinkingClaimStatus status = SmartThinkingClaimStatus::Unknown;
    bool material = true;
    std::vector<std::string> evidence_ids;
};

struct SmartThinkingVerificationAction {
    std::string id;
    std::string capability_family;
    std::string contract_version;
    nlohmann::json typed_input = nlohmann::json::object();
    std::vector<std::string> dependencies;
    SmartThinkingActionStatus status = SmartThinkingActionStatus::Pending;
    bool side_effect_free = true;
};

struct SmartThinkingVerificationCheck {
    std::string id;
    std::string check_type;
    std::vector<std::string> claim_ids;
    std::vector<std::string> action_ids;
    SmartThinkingVerificationTier provided_tier =
        SmartThinkingVerificationTier::Structural;
    SmartThinkingVerificationFailurePolicy failure_policy =
        SmartThinkingVerificationFailurePolicy::Abort;
    SmartThinkingCheckStatus status = SmartThinkingCheckStatus::Pending;
};

struct SmartThinkingVerificationObservation {
    std::string id;
    std::string action_id;
    SmartThinkingVerificationTier tier =
        SmartThinkingVerificationTier::None;
    bool accepted = false;
    nlohmann::json payload = nlohmann::json::object();
    std::string failure_reason;
    std::string state_hash;
};

struct SmartThinkingVerificationPlanIR {
    std::string version = "1";
    std::string goal;
    std::vector<SmartThinkingVerificationClaim> claims;
    std::vector<SmartThinkingVerificationAction> actions;
    std::vector<SmartThinkingVerificationCheck> checks;
    std::size_t max_actions = 32;
    std::size_t max_observations = 64;
    bool allow_side_effects = false;
};

struct SmartThinkingVerificationEvent {
    std::size_t sequence = 0;
    SmartThinkingVerificationEventType type =
        SmartThinkingVerificationEventType::PlanValidated;
    std::string subject_id;
    nlohmann::json payload = nlohmann::json::object();
};

inline bool smart_thinking_verification_identifier_valid(
    const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_')) return false;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (!(std::isalnum(c) || raw == '_' || raw == '-' || raw == '.')) {
            return false;
        }
    }
    return true;
}

inline bool validate_smart_thinking_verification_plan(
    const SmartThinkingVerificationPlanIR& plan,
    std::string* failure_reason = nullptr) {
    const auto fail = [&](const std::string& reason) {
        if (failure_reason != nullptr) *failure_reason = reason;
        return false;
    };
    if (plan.version != "1" || plan.goal.empty() ||
        plan.goal.size() > 4096) {
        return fail("verification_plan_header_invalid");
    }
    if (plan.claims.size() > 128 || plan.actions.size() > plan.max_actions ||
        plan.actions.size() > 64 || plan.checks.size() > 128 ||
        plan.max_observations == 0 || plan.max_observations > 256) {
        return fail("verification_plan_limits_invalid");
    }

    std::set<std::string> claim_ids;
    std::set<std::string> action_ids;
    std::set<std::string> check_ids;
    for (const auto& claim : plan.claims) {
        if (!smart_thinking_verification_identifier_valid(claim.id) ||
            claim.proposition.empty() || claim.proposition.size() > 4096 ||
            !claim_ids.insert(claim.id).second) {
            return fail("verification_claim_invalid:" + claim.id);
        }
        if (claim.status != SmartThinkingClaimStatus::Unknown ||
            !claim.evidence_ids.empty() ||
            (claim.material &&
             claim.required_tier == SmartThinkingVerificationTier::None)) {
            return fail("verification_claim_must_start_unresolved:" + claim.id);
        }
    }
    for (const auto& action : plan.actions) {
        if (!smart_thinking_verification_identifier_valid(action.id) ||
            action.capability_family.empty() ||
            action.contract_version.empty() ||
            !action_ids.insert(action.id).second ||
            action.status != SmartThinkingActionStatus::Pending ||
            (!plan.allow_side_effects && !action.side_effect_free)) {
            return fail("verification_action_invalid:" + action.id);
        }
    }
    for (const auto& action : plan.actions) {
        std::set<std::string> unique_dependencies;
        for (const auto& dependency : action.dependencies) {
            if (dependency == action.id || action_ids.count(dependency) == 0 ||
                !unique_dependencies.insert(dependency).second) {
                return fail("verification_action_dependency_invalid:" +
                            action.id);
            }
        }
    }

    std::map<std::string, int> visit_state;
    std::map<std::string, const SmartThinkingVerificationAction*> by_id;
    for (const auto& action : plan.actions) by_id[action.id] = &action;
    const std::function<bool(const std::string&)> visit =
        [&](const std::string& id) {
            if (visit_state[id] == 1) return false;
            if (visit_state[id] == 2) return true;
            visit_state[id] = 1;
            for (const auto& dependency : by_id.at(id)->dependencies) {
                if (!visit(dependency)) return false;
            }
            visit_state[id] = 2;
            return true;
        };
    for (const auto& action : plan.actions) {
        if (!visit(action.id)) {
            return fail("verification_action_dependency_cycle");
        }
    }

    std::set<std::string> claims_with_checks;
    for (const auto& check : plan.checks) {
        if (!smart_thinking_verification_identifier_valid(check.id) ||
            check.check_type.empty() || !check_ids.insert(check.id).second ||
            check.status != SmartThinkingCheckStatus::Pending) {
            return fail("verification_check_invalid:" + check.id);
        }
        if (check.claim_ids.empty()) {
            return fail("verification_check_claims_missing:" + check.id);
        }
        if (check.action_ids.empty()) {
            return fail("verification_check_actions_missing:" + check.id);
        }
        std::set<std::string> unique_claim_ids;
        for (const auto& claim_id : check.claim_ids) {
            if (!unique_claim_ids.insert(claim_id).second) {
                return fail("verification_check_duplicate_claim:" + claim_id);
            }
            if (claim_ids.count(claim_id) == 0) {
                return fail("verification_check_unknown_claim:" + claim_id);
            }
            const auto claim = std::find_if(
                plan.claims.begin(), plan.claims.end(),
                [&](const SmartThinkingVerificationClaim& candidate) {
                    return candidate.id == claim_id;
                });
            if (claim == plan.claims.end() ||
                static_cast<int>(check.provided_tier) <
                    static_cast<int>(claim->required_tier)) {
                return fail("verification_check_tier_insufficient:" +
                            check.id + ":" + claim_id);
            }
            claims_with_checks.insert(claim_id);
        }
        std::set<std::string> unique_action_ids;
        for (const auto& action_id : check.action_ids) {
            if (action_ids.count(action_id) == 0) {
                return fail("verification_check_unknown_action:" + action_id);
            }
            if (!unique_action_ids.insert(action_id).second) {
                return fail("verification_check_duplicate_action:" + action_id);
            }
        }
    }
    for (const auto& claim : plan.claims) {
        if (claim.material && claims_with_checks.count(claim.id) == 0) {
            return fail("verification_material_claim_unchecked:" + claim.id);
        }
    }
    return true;
}

class SmartThinkingVerificationLedger {
public:
    bool initialize(const SmartThinkingVerificationPlanIR& plan,
                    std::string* failure_reason = nullptr) {
        clear();
        if (!validate_smart_thinking_verification_plan(plan, failure_reason)) {
            return false;
        }
        plan_ = plan;
        for (const auto& claim : plan.claims) claims_[claim.id] = claim;
        for (const auto& action : plan.actions) actions_[action.id] = action;
        for (const auto& check : plan.checks) checks_[check.id] = check;
        append(SmartThinkingVerificationEventType::PlanValidated, "plan",
               {{"goal", plan.goal}, {"version", plan.version}});
        initialized_ = true;
        return true;
    }

    void clear() {
        initialized_ = false;
        completed_ = false;
        aborted_ = false;
        invariant_violations_ = 0;
        plan_ = SmartThinkingVerificationPlanIR{};
        claims_.clear();
        actions_.clear();
        checks_.clear();
        observations_.clear();
        check_evidence_ids_.clear();
        events_.clear();
    }

    bool start_action(const std::string& action_id,
                      std::string* failure_reason = nullptr) {
        auto found = actions_.find(action_id);
        if (!initialized_ || completed_ || aborted_ || found == actions_.end() ||
            found->second.status != SmartThinkingActionStatus::Pending) {
            return invariant_failure("verification_action_start_invalid:" +
                                     action_id, failure_reason);
        }
        for (const auto& dependency : found->second.dependencies) {
            const auto dependency_found = actions_.find(dependency);
            if (dependency_found == actions_.end() ||
                dependency_found->second.status !=
                    SmartThinkingActionStatus::Executed) {
                return invariant_failure(
                    "verification_action_dependency_unsatisfied:" + action_id,
                    failure_reason);
            }
        }
        found->second.status = SmartThinkingActionStatus::Running;
        append(SmartThinkingVerificationEventType::ActionStarted, action_id,
               {{"capability_family", found->second.capability_family},
                {"contract_version", found->second.contract_version}});
        return true;
    }

    bool record_observation(SmartThinkingVerificationObservation observation,
                            std::string* failure_reason = nullptr) {
        auto found = actions_.find(observation.action_id);
        if (!initialized_ || completed_ || aborted_ || found == actions_.end() ||
            found->second.status != SmartThinkingActionStatus::Running ||
            !smart_thinking_verification_identifier_valid(observation.id) ||
            observations_.count(observation.id) != 0 ||
            observations_.size() >= plan_.max_observations) {
            return invariant_failure(
                "verification_observation_invalid:" + observation.id,
                failure_reason);
        }
        if (observation.accepted &&
            (observation.tier == SmartThinkingVerificationTier::None ||
             observation.state_hash.empty())) {
            return invariant_failure(
                "verification_observation_provenance_missing:" +
                    observation.id,
                failure_reason);
        }
        if (!observation.accepted && observation.failure_reason.empty()) {
            return invariant_failure(
                "verification_observation_failure_missing:" +
                    observation.id,
                failure_reason);
        }
        found->second.status = observation.accepted
            ? SmartThinkingActionStatus::Executed
            : SmartThinkingActionStatus::Rejected;
        observations_[observation.id] = observation;
        append(observation.accepted
                   ? SmartThinkingVerificationEventType::ObservationRecorded
                   : SmartThinkingVerificationEventType::ActionRejected,
               observation.action_id,
               {{"observation_id", observation.id},
                {"tier", to_string(observation.tier)},
                {"accepted", observation.accepted},
                {"failure_reason", observation.failure_reason},
                {"state_hash", observation.state_hash}});
        return true;
    }

    bool resolve_check(const std::string& check_id,
                       bool passed,
                       const std::vector<std::string>& evidence_ids,
                       std::string* failure_reason = nullptr) {
        auto found = checks_.find(check_id);
        if (!initialized_ || completed_ || aborted_ || found == checks_.end() ||
            found->second.status != SmartThinkingCheckStatus::Pending ||
            evidence_ids.empty()) {
            return invariant_failure("verification_check_resolution_invalid:" +
                                     check_id, failure_reason);
        }
        for (const auto& action_id : found->second.action_ids) {
            const auto action = actions_.find(action_id);
            if (action == actions_.end() ||
                action->second.status != SmartThinkingActionStatus::Executed) {
                return invariant_failure(
                    "verification_check_action_incomplete:" + action_id,
                    failure_reason);
            }
        }

        SmartThinkingVerificationTier strongest =
            SmartThinkingVerificationTier::None;
        std::set<std::string> unique_evidence;
        const std::set<std::string> allowed_actions(
            found->second.action_ids.begin(), found->second.action_ids.end());
        for (const auto& evidence_id : evidence_ids) {
            if (!unique_evidence.insert(evidence_id).second) {
                return invariant_failure(
                    "verification_check_evidence_duplicate:" + evidence_id,
                    failure_reason);
            }
            const auto observation = observations_.find(evidence_id);
            if (observation == observations_.end() ||
                !observation->second.accepted ||
                allowed_actions.count(observation->second.action_id) == 0) {
                return invariant_failure(
                    "verification_check_evidence_invalid:" + evidence_id,
                    failure_reason);
            }
            if (static_cast<int>(observation->second.tier) >
                static_cast<int>(strongest)) {
                strongest = observation->second.tier;
            }
        }
        if (static_cast<int>(strongest) <
            static_cast<int>(found->second.provided_tier)) {
            return invariant_failure(
                "verification_check_evidence_tier_insufficient:" + check_id,
                failure_reason);
        }
        found->second.status = passed ? SmartThinkingCheckStatus::Passed
                                      : SmartThinkingCheckStatus::Failed;
        check_evidence_ids_[check_id] = evidence_ids;
        append(passed ? SmartThinkingVerificationEventType::CheckPassed
                      : SmartThinkingVerificationEventType::CheckFailed,
               check_id,
               {{"evidence_ids", evidence_ids},
                {"provided_tier", to_string(strongest)}});
        for (const auto& claim_id : found->second.claim_ids) {
            recompute_claim_status(claim_id, check_id);
        }
        return true;
    }

    bool finish(std::string* failure_reason = nullptr) {
        if (!initialized_ || completed_ || aborted_) {
            return invariant_failure("verification_plan_finish_invalid",
                                     failure_reason);
        }
        for (const auto& [id, action] : actions_) {
            if (action.status != SmartThinkingActionStatus::Executed) {
                return invariant_failure(
                    "verification_plan_action_incomplete:" + id,
                    failure_reason);
            }
        }
        for (const auto& [id, check] : checks_) {
            if (check.status != SmartThinkingCheckStatus::Passed) {
                return invariant_failure(
                    "verification_plan_check_incomplete:" + id,
                    failure_reason);
            }
        }
        for (const auto& [id, claim] : claims_) {
            if (claim.material && claim.status != SmartThinkingClaimStatus::Verified) {
                return invariant_failure(
                    "verification_plan_claim_unverified:" + id,
                    failure_reason);
            }
        }
        completed_ = true;
        append(SmartThinkingVerificationEventType::PlanCompleted, "plan",
               {{"verified_claims", verified_claim_count()},
                {"observations", observations_.size()}});
        return true;
    }

    void abort(const std::string& reason) {
        if (!initialized_ || completed_ || aborted_) return;
        aborted_ = true;
        append(SmartThinkingVerificationEventType::PlanAborted, "plan",
               {{"reason", reason}});
    }

    bool completed() const { return completed_; }
    bool aborted() const { return aborted_; }
    std::size_t invariant_violations() const { return invariant_violations_; }
    std::size_t verified_claim_count() const {
        return static_cast<std::size_t>(std::count_if(
            claims_.begin(), claims_.end(), [](const auto& item) {
                return item.second.status == SmartThinkingClaimStatus::Verified;
            }));
    }
    const std::vector<SmartThinkingVerificationEvent>& events() const {
        return events_;
    }

    nlohmann::json to_json() const {
        nlohmann::json claims = nlohmann::json::array();
        for (const auto& [id, claim] : claims_) {
            claims.push_back({
                {"id", id},
                {"proposition", claim.proposition},
                {"required_tier", to_string(claim.required_tier)},
                {"status", to_string(claim.status)},
                {"material", claim.material},
                {"evidence_ids", claim.evidence_ids}
            });
        }
        nlohmann::json actions = nlohmann::json::array();
        for (const auto& [id, action] : actions_) {
            actions.push_back({
                {"id", id},
                {"capability_family", action.capability_family},
                {"contract_version", action.contract_version},
                {"status", to_string(action.status)},
                {"dependencies", action.dependencies},
                {"side_effect_free", action.side_effect_free}
            });
        }
        nlohmann::json checks = nlohmann::json::array();
        for (const auto& [id, check] : checks_) {
            checks.push_back({
                {"id", id},
                {"check_type", check.check_type},
                {"claim_ids", check.claim_ids},
                {"action_ids", check.action_ids},
                {"provided_tier", to_string(check.provided_tier)},
                {"status", to_string(check.status)},
                {"failure_policy", to_string(check.failure_policy)}
            });
        }
        nlohmann::json observations = nlohmann::json::array();
        for (const auto& [id, observation] : observations_) {
            observations.push_back({
                {"id", id},
                {"action_id", observation.action_id},
                {"tier", to_string(observation.tier)},
                {"accepted", observation.accepted},
                {"payload", observation.payload},
                {"failure_reason", observation.failure_reason},
                {"state_hash", observation.state_hash}
            });
        }
        nlohmann::json events = nlohmann::json::array();
        for (const auto& event : events_) {
            events.push_back({
                {"sequence", event.sequence},
                {"type", to_string(event.type)},
                {"subject_id", event.subject_id},
                {"payload", event.payload}
            });
        }
        return {
            {"version", plan_.version},
            {"goal", plan_.goal},
            {"allow_side_effects", plan_.allow_side_effects},
            {"completed", completed_},
            {"aborted", aborted_},
            {"invariant_violations", invariant_violations_},
            {"claims", std::move(claims)},
            {"actions", std::move(actions)},
            {"checks", std::move(checks)},
            {"observations", std::move(observations)},
            {"events", std::move(events)}
        };
    }

private:
    void recompute_claim_status(const std::string& claim_id,
                                const std::string& triggering_check_id) {
        auto& claim = claims_.at(claim_id);
        bool has_check = false;
        bool all_passed = true;
        bool abort_failure = false;
        bool unresolved_failure = false;
        std::set<std::string> evidence;
        for (const auto& [check_id, check] : checks_) {
            if (std::find(check.claim_ids.begin(), check.claim_ids.end(),
                          claim_id) == check.claim_ids.end()) {
                continue;
            }
            has_check = true;
            if (check.status == SmartThinkingCheckStatus::Pending ||
                check.status == SmartThinkingCheckStatus::Skipped) {
                all_passed = false;
            } else if (check.status == SmartThinkingCheckStatus::Failed) {
                all_passed = false;
                if (check.failure_policy ==
                    SmartThinkingVerificationFailurePolicy::Abort) {
                    abort_failure = true;
                } else {
                    unresolved_failure = true;
                }
            }
            const auto evidence_found = check_evidence_ids_.find(check_id);
            if (evidence_found != check_evidence_ids_.end()) {
                evidence.insert(evidence_found->second.begin(),
                                evidence_found->second.end());
            }
        }

        SmartThinkingClaimStatus updated = SmartThinkingClaimStatus::Unknown;
        SmartThinkingVerificationEventType event_type =
            SmartThinkingVerificationEventType::ClaimUnresolved;
        bool emit = false;
        if (abort_failure) {
            updated = SmartThinkingClaimStatus::Refuted;
            event_type = SmartThinkingVerificationEventType::ClaimRefuted;
            emit = claim.status != updated;
        } else if (unresolved_failure) {
            updated = SmartThinkingClaimStatus::Unresolved;
            event_type = SmartThinkingVerificationEventType::ClaimUnresolved;
            emit = claim.status != updated;
        } else if (has_check && all_passed) {
            updated = SmartThinkingClaimStatus::Verified;
            event_type = SmartThinkingVerificationEventType::ClaimVerified;
            emit = claim.status != updated;
        }
        claim.status = updated;
        claim.evidence_ids.assign(evidence.begin(), evidence.end());
        if (emit) {
            append(event_type, claim_id,
                   {{"check_id", triggering_check_id},
                    {"evidence_ids", claim.evidence_ids}});
        }
    }

    bool invariant_failure(const std::string& reason,
                           std::string* failure_reason) {
        ++invariant_violations_;
        if (failure_reason != nullptr) *failure_reason = reason;
        return false;
    }

    void append(SmartThinkingVerificationEventType type,
                std::string subject_id,
                nlohmann::json payload) {
        events_.push_back({events_.size(), type, std::move(subject_id),
                           std::move(payload)});
    }

    bool initialized_ = false;
    bool completed_ = false;
    bool aborted_ = false;
    std::size_t invariant_violations_ = 0;
    SmartThinkingVerificationPlanIR plan_;
    std::map<std::string, SmartThinkingVerificationClaim> claims_;
    std::map<std::string, SmartThinkingVerificationAction> actions_;
    std::map<std::string, SmartThinkingVerificationCheck> checks_;
    std::map<std::string, SmartThinkingVerificationObservation> observations_;
    std::map<std::string, std::vector<std::string>> check_evidence_ids_;
    std::vector<SmartThinkingVerificationEvent> events_;
};

}  // namespace lemon
