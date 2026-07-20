#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

// Search-controller primitives intentionally kept independent from the
// Lemonade backend and prompt implementation. They mirror the useful split in
// LLM Reasoners (transition model, expansion policy, search strategy) while
// enforcing OpenHands-style append-only execution events.
enum class SmartThinkingCompileStatus {
    NoMatch,
    Compiled,
    Rejected
};

inline const char* to_string(SmartThinkingCompileStatus status) {
    switch (status) {
        case SmartThinkingCompileStatus::NoMatch: return "no_match";
        case SmartThinkingCompileStatus::Compiled: return "compiled";
        case SmartThinkingCompileStatus::Rejected: return "rejected";
    }
    return "unknown";
}

enum class SmartThinkingPruneReason {
    None,
    ModelVerifier,
    ParseFailure,
    StructuralFailure,
    DeterministicValidationFailure,
    BudgetExceeded
};

enum class SmartThinkingLineageStatus {
    Created,
    Active,
    Validated,
    Rejected,
    Terminal
};

enum class SmartThinkingSearchEventType {
    LineageCreated,
    ActionProposed,
    ObservationProduced,
    StateValidated,
    StateRejected,
    LineagePruned,
    ReplacementSpawned,
    TerminalReached,
    CandidateAuditReused,
    CandidateSelected,
    BudgetExhausted
};

inline const char* to_string(SmartThinkingPruneReason reason) {
    switch (reason) {
        case SmartThinkingPruneReason::None: return "none";
        case SmartThinkingPruneReason::ModelVerifier: return "model_verifier";
        case SmartThinkingPruneReason::ParseFailure: return "parse_failure";
        case SmartThinkingPruneReason::StructuralFailure: return "structural_failure";
        case SmartThinkingPruneReason::DeterministicValidationFailure:
            return "deterministic_validation_failure";
        case SmartThinkingPruneReason::BudgetExceeded: return "budget_exceeded";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingLineageStatus status) {
    switch (status) {
        case SmartThinkingLineageStatus::Created: return "created";
        case SmartThinkingLineageStatus::Active: return "active";
        case SmartThinkingLineageStatus::Validated: return "validated";
        case SmartThinkingLineageStatus::Rejected: return "rejected";
        case SmartThinkingLineageStatus::Terminal: return "terminal";
    }
    return "unknown";
}

inline const char* to_string(SmartThinkingSearchEventType type) {
    switch (type) {
        case SmartThinkingSearchEventType::LineageCreated: return "lineage_created";
        case SmartThinkingSearchEventType::ActionProposed: return "action_proposed";
        case SmartThinkingSearchEventType::ObservationProduced: return "observation_produced";
        case SmartThinkingSearchEventType::StateValidated: return "state_validated";
        case SmartThinkingSearchEventType::StateRejected: return "state_rejected";
        case SmartThinkingSearchEventType::LineagePruned: return "lineage_pruned";
        case SmartThinkingSearchEventType::ReplacementSpawned: return "replacement_spawned";
        case SmartThinkingSearchEventType::TerminalReached: return "terminal_reached";
        case SmartThinkingSearchEventType::CandidateAuditReused:
            return "candidate_audit_reused";
        case SmartThinkingSearchEventType::CandidateSelected: return "candidate_selected";
        case SmartThinkingSearchEventType::BudgetExhausted: return "budget_exhausted";
    }
    return "unknown";
}

inline std::string smart_thinking_state_fingerprint(const nlohmann::json& value) {
    const std::string input = value.dump();
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : input) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

struct SmartThinkingSearchBudget {
    // Negative means unlimited; zero explicitly forbids model proposals.
    int max_model_calls = -1;
    int max_tool_calls = 0;
    int max_steps = 0;
    std::int64_t max_prompt_tokens = 0;
    std::int64_t max_completion_tokens = 0;
    std::int64_t max_wall_time_ms = 0;
    std::int64_t reserved_finalization_tokens = 0;
};

struct SmartThinkingTransitionResult {
    bool accepted = false;
    bool terminal = false;
    nlohmann::json next_state = nlohmann::json::object();
    nlohmann::json observation = nlohmann::json::object();
    std::string failure_reason;
};

class ISmartThinkingTransitionModel {
public:
    virtual ~ISmartThinkingTransitionModel() = default;

    virtual nlohmann::json initial_state(
        const nlohmann::json& compiled_task) const = 0;
    virtual SmartThinkingTransitionResult step(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state,
        const nlohmann::json& action) const = 0;
    virtual bool is_terminal(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state) const = 0;
};

class ISmartThinkingExpansionPolicy {
public:
    virtual ~ISmartThinkingExpansionPolicy() = default;

    // Most policies call a model to propose actions. Deterministic policies may
    // override this so model-call budgets and evaluation accounting remain
    // truthful.
    virtual bool consumes_model_call() const {
        return true;
    }

    virtual std::vector<nlohmann::json> propose_actions(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state,
        const SmartThinkingSearchBudget& budget) = 0;
};

struct SmartThinkingStrategyResult {
    bool completed = false;
    nlohmann::json terminal_state = nlohmann::json::object();
    std::string stop_reason;
    int proposal_calls = 0;
    int model_calls = 0;
    int transitions_attempted = 0;
};

class ISmartThinkingSearchStrategy {
public:
    virtual ~ISmartThinkingSearchStrategy() = default;

    virtual SmartThinkingStrategyResult run(
        const nlohmann::json& compiled_task,
        ISmartThinkingTransitionModel& transition_model,
        ISmartThinkingExpansionPolicy& expansion_policy,
        const SmartThinkingSearchBudget& budget) = 0;
};

struct SmartThinkingSearchEvent {
    std::uint64_t sequence = 0;
    SmartThinkingSearchEventType type = SmartThinkingSearchEventType::LineageCreated;
    std::string lineage_id;
    std::string parent_lineage_id;
    std::string state_hash;
    std::string parent_state_hash;
    nlohmann::json payload = nlohmann::json::object();
    bool invariant_violation = false;
    std::string invariant_failure;
};

// The log never mutates or removes prior events. It also detects attempts to
// continue a rejected lineage. Detection is deliberately non-throwing so debug
// instrumentation cannot take down inference; callers can fail tests or stop
// search based on invariant_violations().
class SmartThinkingSearchEventLog {
public:
    void clear() {
        events_.clear();
        lineage_status_.clear();
        invariant_violations_ = 0;
        next_sequence_ = 0;
    }

    const SmartThinkingSearchEvent& append(SmartThinkingSearchEvent event) {
        event.sequence = next_sequence_++;

        const auto found = lineage_status_.find(event.lineage_id);
        if (found != lineage_status_.end() &&
            found->second == SmartThinkingLineageStatus::Rejected) {
            event.invariant_violation = true;
            event.invariant_failure = "rejected_lineage_received_descendant_event";
            ++invariant_violations_;
        }

        // Preserve the last valid status when a caller attempts an illegal
        // descendant transition. The event remains in the append-only log for
        // diagnosis, but it must never resurrect or otherwise mutate a rejected
        // lineage.
        if (!event.invariant_violation) {
            switch (event.type) {
                case SmartThinkingSearchEventType::LineageCreated:
                case SmartThinkingSearchEventType::ActionProposed:
                    lineage_status_[event.lineage_id] = SmartThinkingLineageStatus::Active;
                    break;
                case SmartThinkingSearchEventType::StateValidated:
                    lineage_status_[event.lineage_id] = SmartThinkingLineageStatus::Validated;
                    break;
                case SmartThinkingSearchEventType::StateRejected:
                case SmartThinkingSearchEventType::LineagePruned:
                    lineage_status_[event.lineage_id] = SmartThinkingLineageStatus::Rejected;
                    break;
                case SmartThinkingSearchEventType::TerminalReached:
                    lineage_status_[event.lineage_id] = SmartThinkingLineageStatus::Terminal;
                    break;
                case SmartThinkingSearchEventType::ObservationProduced:
                case SmartThinkingSearchEventType::ReplacementSpawned:
                case SmartThinkingSearchEventType::CandidateAuditReused:
                case SmartThinkingSearchEventType::CandidateSelected:
                case SmartThinkingSearchEventType::BudgetExhausted:
                    break;
            }
        }

        events_.push_back(std::move(event));
        return events_.back();
    }

    const std::vector<SmartThinkingSearchEvent>& events() const {
        return events_;
    }

    std::size_t invariant_violations() const {
        return invariant_violations_;
    }

    nlohmann::json to_json() const {
        nlohmann::json result = nlohmann::json::array();
        for (const auto& event : events_) {
            result.push_back({
                {"sequence", event.sequence},
                {"type", to_string(event.type)},
                {"lineage_id", event.lineage_id},
                {"parent_lineage_id", event.parent_lineage_id},
                {"state_hash", event.state_hash},
                {"parent_state_hash", event.parent_state_hash},
                {"payload", event.payload},
                {"invariant_violation", event.invariant_violation},
                {"invariant_failure", event.invariant_failure}
            });
        }
        return result;
    }

private:
    std::vector<SmartThinkingSearchEvent> events_;
    std::map<std::string, SmartThinkingLineageStatus> lineage_status_;
    std::size_t invariant_violations_ = 0;
    std::uint64_t next_sequence_ = 0;
};

// Minimal executable reference strategy. The expansion policy orders actions;
// greedy search selects the first action, while the transition model alone
// computes the next state. Model-provided claims about a post-state are never
// trusted. This intentionally small loop is the complexity baseline before
// beam search or MCTS are introduced.
class SmartThinkingVerifiedGreedySearch final : public ISmartThinkingSearchStrategy {
public:
    explicit SmartThinkingVerifiedGreedySearch(
        SmartThinkingSearchEventLog* event_log = nullptr)
        : external_event_log_(event_log) {}

    SmartThinkingStrategyResult run(
        const nlohmann::json& compiled_task,
        ISmartThinkingTransitionModel& transition_model,
        ISmartThinkingExpansionPolicy& expansion_policy,
        const SmartThinkingSearchBudget& budget) override {
        SmartThinkingSearchEventLog local_event_log;
        SmartThinkingSearchEventLog& event_log =
            external_event_log_ != nullptr ? *external_event_log_ : local_event_log;
        event_log.clear();

        SmartThinkingStrategyResult result;
        if (budget.max_steps <= 0) {
            result.stop_reason = "invalid_step_budget";
            return result;
        }

        constexpr const char* lineage_id = "verified-greedy-1";
        nlohmann::json state = transition_model.initial_state(compiled_task);
        std::string state_hash = smart_thinking_state_fingerprint(state);

        SmartThinkingSearchEvent created;
        created.type = SmartThinkingSearchEventType::LineageCreated;
        created.lineage_id = lineage_id;
        created.state_hash = state_hash;
        created.payload = {{"strategy", "verified_greedy"}};
        event_log.append(std::move(created));

        if (transition_model.is_terminal(compiled_task, state)) {
            SmartThinkingSearchEvent terminal;
            terminal.type = SmartThinkingSearchEventType::TerminalReached;
            terminal.lineage_id = lineage_id;
            terminal.state_hash = state_hash;
            terminal.payload = {{"initial_state_terminal", true}};
            event_log.append(std::move(terminal));
            result.completed = true;
            result.terminal_state = std::move(state);
            result.stop_reason = "terminal_initial_state";
            return result;
        }

        const auto started_at = std::chrono::steady_clock::now();
        for (int step_index = 0; step_index < budget.max_steps; ++step_index) {
            if (budget.max_wall_time_ms > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started_at).count();
                if (elapsed > budget.max_wall_time_ms) {
                    SmartThinkingSearchEvent exhausted;
                    exhausted.type = SmartThinkingSearchEventType::BudgetExhausted;
                    exhausted.lineage_id = lineage_id;
                    exhausted.state_hash = state_hash;
                    exhausted.payload = {{"budget", "wall_time_ms"},
                                         {"elapsed_ms", elapsed}};
                    event_log.append(std::move(exhausted));
                    result.stop_reason = "wall_time_budget_exhausted";
                    return result;
                }
            }
            const bool proposal_consumes_model_call =
                expansion_policy.consumes_model_call();
            if (proposal_consumes_model_call && budget.max_model_calls >= 0 &&
                result.model_calls >= budget.max_model_calls) {
                SmartThinkingSearchEvent exhausted;
                exhausted.type = SmartThinkingSearchEventType::BudgetExhausted;
                exhausted.lineage_id = lineage_id;
                exhausted.state_hash = state_hash;
                exhausted.payload = {{"budget", "model_calls"}};
                event_log.append(std::move(exhausted));
                result.stop_reason = "model_call_budget_exhausted";
                return result;
            }

            const auto actions = expansion_policy.propose_actions(
                compiled_task, state, budget);
            ++result.proposal_calls;
            if (proposal_consumes_model_call) ++result.model_calls;
            if (actions.empty()) {
                SmartThinkingSearchEvent pruned;
                pruned.type = SmartThinkingSearchEventType::LineagePruned;
                pruned.lineage_id = lineage_id;
                pruned.state_hash = state_hash;
                pruned.payload = {{"reason", "no_actions"}};
                event_log.append(std::move(pruned));
                result.stop_reason = "no_actions";
                return result;
            }

            const nlohmann::json& action = actions.front();
            SmartThinkingSearchEvent proposed;
            proposed.type = SmartThinkingSearchEventType::ActionProposed;
            proposed.lineage_id = lineage_id;
            proposed.state_hash = state_hash;
            proposed.payload = {{"step", step_index}, {"action", action}};
            event_log.append(std::move(proposed));

            ++result.transitions_attempted;
            SmartThinkingTransitionResult transition = transition_model.step(
                compiled_task, state, action);
            if (!transition.accepted) {
                SmartThinkingSearchEvent rejected;
                rejected.type = SmartThinkingSearchEventType::StateRejected;
                rejected.lineage_id = lineage_id;
                rejected.state_hash = state_hash;
                rejected.payload = {
                    {"step", step_index},
                    {"reason", transition.failure_reason},
                    {"observation", transition.observation}
                };
                event_log.append(std::move(rejected));
                result.stop_reason = transition.failure_reason.empty()
                    ? "transition_rejected"
                    : "transition_rejected:" + transition.failure_reason;
                return result;
            }

            const std::string parent_hash = state_hash;
            state = std::move(transition.next_state);
            state_hash = smart_thinking_state_fingerprint(state);

            SmartThinkingSearchEvent observation;
            observation.type = SmartThinkingSearchEventType::ObservationProduced;
            observation.lineage_id = lineage_id;
            observation.parent_state_hash = parent_hash;
            observation.state_hash = state_hash;
            observation.payload = {
                {"step", step_index},
                {"observation", transition.observation}
            };
            event_log.append(std::move(observation));

            SmartThinkingSearchEvent validated;
            validated.type = SmartThinkingSearchEventType::StateValidated;
            validated.lineage_id = lineage_id;
            validated.parent_state_hash = parent_hash;
            validated.state_hash = state_hash;
            validated.payload = {
                {"step", step_index},
                {"observation", transition.observation}
            };
            event_log.append(std::move(validated));

            if (transition.terminal ||
                transition_model.is_terminal(compiled_task, state)) {
                SmartThinkingSearchEvent terminal;
                terminal.type = SmartThinkingSearchEventType::TerminalReached;
                terminal.lineage_id = lineage_id;
                terminal.parent_state_hash = parent_hash;
                terminal.state_hash = state_hash;
                terminal.payload = {{"step", step_index}};
                event_log.append(std::move(terminal));
                result.completed = true;
                result.terminal_state = std::move(state);
                result.stop_reason = "terminal_state_reached";
                return result;
            }
        }

        SmartThinkingSearchEvent exhausted;
        exhausted.type = SmartThinkingSearchEventType::BudgetExhausted;
        exhausted.lineage_id = lineage_id;
        exhausted.state_hash = state_hash;
        exhausted.payload = {{"budget", "steps"}};
        event_log.append(std::move(exhausted));
        result.stop_reason = "step_budget_exhausted";
        return result;
    }

private:
    SmartThinkingSearchEventLog* external_event_log_ = nullptr;
};

}  // namespace lemon
