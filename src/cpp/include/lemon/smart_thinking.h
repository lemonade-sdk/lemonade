#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/smart_thinking_capability.h"
#include "lemon/smart_thinking_search.h"

namespace lemon {

using json = nlohmann::json;

enum class SmartThinkingMode {
    Off,
    Auto,
    Deep
};

enum class SmartThinkingCritic {
    Same,
    Router
};

enum class SmartThinkingCloudAssist {
    Never,
    Auto,
    Verify
};

enum class SmartThinkingToolPolicy {
    Bypass,
    Plan
};

enum class SmartThinkingStep {
    MetaPlan,
    BranchGeneration,
    SelfReview,
    Revision,
    Critic,
    Adjudication,
    Verifier,
    Final
};

enum class SmartThinkingBackendBatching {
    Sequential,
    NativeBatch,
    PrefixCache
};

enum class SmartThinkingSelectionPolicy {
    Verifier,
    IndependentReference
};

// LegacySearch preserves the historical prompt-based branch controller for
// controlled comparisons. VerifiedAuto first attempts a deterministic kernel
// and otherwise uses one native trajectory, avoiding progressive overhead on
// unsupported or ambiguous tasks.
enum class SmartThinkingExecutionPolicy {
    LegacySearch,
    VerifiedAuto,
    VerifiedRequired
};

// Product-facing presets are derived from the existing request fields rather
// than introducing another public knob. They are reported in diagnostics so a
// client can confirm the exact server behavior it requested.
enum class SmartThinkingProductTier {
    Disabled,
    Smart,
    SmartExtra,
    StrictVerified,
    CustomVerified,
    ExperimentalLegacy
};

struct SmartThinkingConfig {
    SmartThinkingMode mode = SmartThinkingMode::Off;
    // 0 = one native pass, 1 = one checkpoint + terminal branch layer,
    // 2 = two checkpoint layers with bounded beam pruning.
    int budget = 0;
    // Maximum search breadth per layer. The controller may retain a smaller
    // diverse beam after process verification.
    int branches = 3;
    // Maximum witness-guided semantic repairs. -1 derives the default from
    // budget (one for budget 1, two for budget 2). Zero keeps generation and
    // selection measurable without any repair calls.
    int repair_budget = -1;
    // Verifier uses per-candidate same-model audits. IndependentReference
    // generates one candidate-blind fresh solution only when unique terminal
    // branches disagree, then selects a matching branch when possible.
    SmartThinkingSelectionPolicy selection_policy = SmartThinkingSelectionPolicy::Verifier;
    // Keep the historical default for API compatibility. Product presets send
    // verified_auto explicitly, while legacy_search remains available to
    // existing experimental clients.
    SmartThinkingExecutionPolicy execution_policy = SmartThinkingExecutionPolicy::LegacySearch;
    SmartThinkingCritic critic = SmartThinkingCritic::Same;
    SmartThinkingCloudAssist cloud_assist = SmartThinkingCloudAssist::Never;
    // Plan performs hidden, tool-disabled deliberation before exactly one real
    // tool-capable model call. Bypass preserves native tool routing. Non-tool
    // fresh-context search never executes tools internally.
    SmartThinkingToolPolicy tool_policy = SmartThinkingToolPolicy::Bypass;
    bool debug = false;
    bool explicitly_present = false;

    bool enabled_for_request(const json& request) const;
    SmartThinkingProductTier product_tier() const;
    // Prepare the single native request used when the verified router abstains
    // or when native tool routing is bypassed. Product tiers may adjust only
    // the completion-token ceiling; tool bypass remains byte-equivalent after
    // removing smart_thinking.
    json native_passthrough_request(const json& request) const;

    static SmartThinkingConfig disabled();
    static std::optional<SmartThinkingConfig> from_request(const json& request,
                                                           json* error = nullptr);
    static json strip_request_fields(json request);
};

struct SmartThinkingCandidate {
    int index = 0;
    std::string text;
    json response;

    // Extracted answer artifact. Hidden reasoning is never copied here unless it
    // contains the explicit result envelope and visible content is empty.
    std::string answer;
    std::string canonical_answer;
    // Model-generated compact signature of the core conclusion. It is used
    // only as a weak agreement signal for open-ended tasks, never as proof.
    std::string consensus_key;
    std::string canonical_consensus_key;
    std::string strategy;
    std::string finish_reason;
    std::string validation_failure;
    bool valid = false;
    bool complete = true;
    int revised_from_index = -1;
    std::vector<std::string> resolved_ticket_ids;

    // Retained for source compatibility with the first MVP API. Revision is
    // now selective and ticket-driven rather than a fixed self-review loop.
    std::string initial_text;
    std::string review_text;
    bool self_reviewed = false;
    bool revised = false;
};

struct SmartThinkingCriticResult {
    int selected_index = 0;
    double confidence = 0.0;
    json scores = json::array();
    bool parsed = false;
    std::string fallback_reason;
    std::string final_answer;
    bool final_answer_valid = false;
    bool verifier_applicable = false;
    bool verifier_found_valid = false;
    int verifier_best_score = 0;
    std::string verifier_summary;
};

struct SmartThinkingUsage {
    int internal_calls = 0;
    bool saw_usage = false;
    long long prompt_tokens = 0;
    long long completion_tokens = 0;
    long long total_tokens = 0;
    long long final_response_tokens = 0;
};

struct SmartThinkingOutputRequirements {
    bool json_only = false;
    bool no_markdown = false;
    std::vector<std::string> required_json_keys;
    // OpenAI response_format JSON schema, when present. Only deterministic,
    // side-effect-free validation is performed by the orchestrator.
    json json_schema = json::object();
};

struct SmartThinkingValidationResult {
    bool valid = false;
    bool repaired = false;
    std::string text;
    std::string failure_reason;
};

class SmartThinkingOrchestrator {
public:
    using GenerateFn = std::function<json(const json&)>;

    explicit SmartThinkingOrchestrator(GenerateFn generator,
                                       GenerateFn judge_generator = GenerateFn{});

    json run(const json& request, const SmartThinkingConfig& config);

    // Public seams retained for unit tests and future backend-native batching.
    std::vector<SmartThinkingCandidate> generate_candidates(
        const json& request,
        const SmartThinkingConfig& config);
    SmartThinkingCriticResult score_candidates(
        const json& request,
        const SmartThinkingConfig& config,
        const std::vector<SmartThinkingCandidate>& candidates);
    json finalize_answer(const json& request,
                         const SmartThinkingConfig& config,
                         const SmartThinkingCandidate& selected,
                         int revision_round);

    static SmartThinkingCriticResult parse_critic_response(const std::string& text,
                                                           int candidate_count);
    static std::string extract_assistant_text(const json& response);
    static std::string extract_visible_assistant_text(const json& response);
    static json make_invalid_config_error(const std::string& message);
    static SmartThinkingOutputRequirements infer_output_requirements(
        const json& request);
    static SmartThinkingValidationResult validate_final_text(
        const std::string& text,
        const SmartThinkingOutputRequirements& requirements);
    static SmartThinkingValidationResult verify_structured_final_text(
        const std::string& text,
        const json& request,
        const SmartThinkingOutputRequirements& requirements);

private:
    struct MetaPlan {
        std::vector<std::string> success_criteria;
        std::vector<std::string> likely_failure_modes;
        std::vector<std::string> useful_representations;
        std::vector<std::string> verification_actions;
        int estimated_difficulty = 0;
        bool parsed = false;
    };

    struct CritiqueTicket {
        std::string id;
        int candidate_index = -1;
        std::string severity;
        std::string category;
        std::string target_claim;
        std::string issue;
        std::string falsification_test;
        std::string evidence;
        bool actionable = false;
        bool disagreement_supported = false;
        bool probe_confirmed = false;
        std::string probe_outcome;
        std::string probe_evidence;
    };

    struct CritiqueReport {
        std::vector<CritiqueTicket> tickets;
        bool parsed = false;
        std::string failure_reason;
    };

    struct TaskProfile {
        int complexity_score = 0;
        bool structured_output = false;
        bool closed_answer = false;
        bool open_ended = true;
        bool has_tools = false;
        bool high_constraint_density = false;
        bool factual_risk = false;
        bool implementation_risk = false;
        bool multi_step_reasoning = false;
        std::string activation_reason;
    };

    struct ComputePlan {
        int min_candidates = 1;
        int max_candidates = 1;
        int max_internal_calls = 1;
        double consensus_threshold = 1.0;
        bool allow_aggregation = false;
        bool require_final_audit = false;
        bool use_meta_plan = false;
        bool use_critique = false;
        bool allow_targeted_revision = false;
        bool use_dispute_probes = false;
        int max_actionable_tickets = 0;
        int max_probe_calls = 0;
    };

    struct ConsensusState {
        int valid_candidates = 0;
        int unique_answers = 0;
        int top_votes = 0;
        int second_votes = 0;
        int exact_top_votes = 0;
        double top_share = 0.0;
        std::string top_key;
        int representative_index = -1;
        bool sampling_stable = false;
        bool exact_answer_consensus = false;
        bool decisive = false;
    };


    struct ToolPlan {
        int index = 0;
        bool should_call_tool = false;
        std::string tool_name;
        json arguments = json::object();
        std::string goal;
        json checks = json::array();
        std::string canonical_key;
        bool parsed = false;
    };

    struct ParsedArtifact {
        std::string answer;
        std::string consensus_key;
        std::string rationale_summary;
        json checks = json::array();
        json uncertainties = json::array();
        bool parsed_envelope = false;
    };

    // V6 deliberately separates disagreement discovery from deciding which
    // answer wins. The frame names one minimal, checkable proposition but is
    // forbidden from selecting a candidate. Blind verifiers then see neutral
    // A/B labels; a challenger may replace the primary only when the result is
    // stable after swapping those labels.
    struct DisputeFrame {
        bool parsed = false;
        bool checkable = false;
        std::string primary_claim;
        std::string challenger_claim;
        std::string discriminating_test;
        std::string evidence_scope;
        std::string failure_reason;
    };

    struct BlindVerdict {
        bool parsed = false;
        std::string supported_label;
        std::string test_result;
        std::string witness;
        std::string failure_reason;
    };

    // V8 searches over compact, public-safe reasoning checkpoints instead of
    // comparing only complete answers. Every child is generated in a fresh
    // model context from the original task plus this checkpoint. The state is
    // deliberately a compact work ledger, not hidden chain-of-thought.
    struct SearchState {
        int id = -1;
        int parent_id = -1;
        int depth = 0;
        int branch_index = 0;
        std::string representation;
        std::string branch_mode;
        std::string state_summary;
        std::vector<std::string> established;
        std::vector<std::string> unresolved;
        std::vector<std::string> invariants;
        std::string next_action;
        json work_state = json::object();
        double progress_fraction = 0.0;
        std::string final_answer;
        std::string canonical_answer;
        json response;
        bool parsed = false;
        bool terminal = false;
        bool valid = false;
        bool output_normalized = false;
        bool pruned = false;
        SmartThinkingPruneReason prune_reason = SmartThinkingPruneReason::None;
        SmartThinkingLineageStatus lineage_status = SmartThinkingLineageStatus::Created;
        std::string lineage_id;
        std::string lineage_origin;
        std::string parent_lineage_id;
        std::string replacement_of;
        std::string state_hash;
        std::string parent_state_hash;
        bool independent_generation = true;
        bool synthetic_reuse = false;
        bool audit_reused = false;
        bool root_trusted = false;
        bool repaired = false;
        // Exact duplicate terminal answers reuse the first audit rather than
        // spending another model call. The duplicate remains visible for
        // agreement and branch-mode diagnostics.
        bool verifier_reused = false;
        int duplicate_of_state_id = -1;
        // Root generation has two bounded recovery stages. If both model
        // serializations fail, a deterministic start-of-task bootstrap keeps
        // fresh-context search alive without inventing task facts.
        bool bootstrap_root = false;
        std::string recovery_mode;
        std::string recovery_failure;
        int repair_parent_id = -1;
        std::string repair_ticket;
        bool repair_ticket_confirmed = false;
        std::string repair_ticket_confirmation_status;
        std::string repair_ticket_confirmation_witness;
        bool repair_ticket_resolved = false;
        std::string repair_resolution_witness;
        std::string validation_failure;
        std::vector<double> verifier_scores;
        std::vector<std::string> verifier_statuses;
        std::vector<std::string> verifier_witnesses;
        std::vector<std::string> verifier_first_errors;
        std::vector<std::string> verifier_tests;
        std::vector<std::string> verifier_recommendations;
        std::vector<std::string> verifier_failures;
        std::vector<std::string> verifier_raw_outputs;
        std::vector<bool> verifier_hard_prune_votes;
        std::vector<std::string> verifier_error_signatures;
        double robust_score = 0.0;
    };


    struct RepairTicketVerdict {
        bool parsed = false;
        std::string status;  // confirmed, rejected, abstain
        int confidence = 0;
        std::string ticket_claim;
        std::string replay_test;
        std::string replay_result;
        std::string confirmation_witness;
        std::string failure_reason;
    };

    struct ProcessVerdict {
        bool parsed = false;
        std::string status;  // accept, reject, abstain
        int confidence = 0;
        std::string raw_output;
        int logical_soundness = 0;
        int constraint_coverage = 0;
        int progress = 0;
        int testability = 0;
        bool fatal_error = false;
        std::string first_error;
        std::string falsification_test;
        std::string witness;
        std::string recommended_next_action;
        bool ticket_resolved = false;
        std::string ticket_resolution_witness;
        double score = 0.0;
        bool hard_prune_supported = false;
        std::string error_signature;
        std::string failure_reason;
    };

    GenerateFn generator_;
    GenerateFn judge_generator_;
    bool has_injected_judge_ = false;

    TaskProfile classify_task(const json& request) const;
    ComputePlan build_plan(const json& request,
                           const SmartThinkingConfig& config,
                           const TaskProfile& profile) const;
    ComputePlan refine_plan_with_meta(const SmartThinkingConfig& config,
                                      const TaskProfile& profile,
                                      ComputePlan plan,
                                      const MetaPlan& meta_plan) const;

    json make_candidate_request(const json& request,
                                const SmartThinkingConfig& config,
                                int branch_index,
                                const TaskProfile& profile,
                                const MetaPlan& meta_plan) const;
    json make_meta_plan_request(const json& request,
                                const SmartThinkingConfig& config,
                                const TaskProfile& profile) const;
    json make_critique_request(
        const json& request,
        const SmartThinkingConfig& config,
        const MetaPlan& meta_plan,
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus) const;
    json make_probe_request(const json& request,
                            const SmartThinkingConfig& config,
                            const CritiqueTicket& ticket) const;
    json make_tool_plan_request(const json& request,
                                const SmartThinkingConfig& config,
                                int plan_index) const;
    json make_tool_adjudication_request(const json& request,
                                        const SmartThinkingConfig& config,
                                        const std::vector<ToolPlan>& plans) const;
    json make_targeted_revision_request(
        const json& request,
        const SmartThinkingConfig& config,
        const MetaPlan& meta_plan,
        const SmartThinkingCandidate& candidate,
        const std::vector<CritiqueTicket>& tickets) const;
    json make_aggregation_request(
        const json& request,
        const SmartThinkingConfig& config,
        const MetaPlan& meta_plan,
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus,
        const std::vector<CritiqueTicket>& tickets) const;
    json make_verification_request(
        const json& request,
        const SmartThinkingConfig& config,
        const std::string& proposed_final,
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus) const;
    json make_repair_request(const json& request,
                             const SmartThinkingConfig& config,
                             const std::string& previous_text,
                             const std::string& validation_failure) const;
    json make_dispute_frame_request(const json& request,
                                    const SmartThinkingConfig& config,
                                    const SmartThinkingCandidate& primary,
                                    const SmartThinkingCandidate& challenger) const;
    json make_blind_verification_request(const json& request,
                                         const SmartThinkingConfig& config,
                                         const DisputeFrame& frame,
                                         bool swap_labels) const;
    json make_root_search_state_request(const json& request,
                                        const SmartThinkingConfig& config,
                                        const TaskProfile& profile) const;
    json make_root_search_state_retry_request(
        const json& request,
        const SmartThinkingConfig& config,
        const std::string& prior_failure) const;
    json make_search_expansion_request(const json& request,
                                       const SmartThinkingConfig& config,
                                       const SearchState& parent,
                                       int child_index,
                                       int target_depth,
                                       bool require_terminal,
                                       bool replacement_restart,
                                       const std::string& replacement_of) const;
    json make_process_verifier_request(const json& request,
                                       const SmartThinkingConfig& config,
                                       const SearchState& state,
                                       int verifier_index) const;
    json make_repair_ticket_confirmation_request(
        const json& request,
        const SmartThinkingConfig& config,
        const SearchState& candidate,
        const std::string& repair_ticket,
        int confirmation_index) const;
    json make_search_repair_request(const json& request,
                                    const SmartThinkingConfig& config,
                                    const SearchState& root,
                                    const SearchState& candidate,
                                    const std::string& repair_ticket,
                                    const RepairTicketVerdict& confirmation,
                                    int repair_index) const;
    json make_search_state_finalizer_request(const json& request,
                                             const SmartThinkingConfig& config,
                                             const std::string& private_reasoning,
                                             bool require_terminal) const;

    SmartThinkingCandidate generate_one_candidate(
        const json& request,
        const SmartThinkingConfig& config,
        const TaskProfile& profile,
        const MetaPlan& meta_plan,
        int branch_index);
    MetaPlan generate_meta_plan(const json& request,
                                const SmartThinkingConfig& config,
                                const TaskProfile& profile);
    CritiqueReport critique_candidates(
        const json& request,
        const SmartThinkingConfig& config,
        const MetaPlan& meta_plan,
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus,
        int max_actionable_tickets);
    bool probe_ticket(const json& request,
                      const SmartThinkingConfig& config,
                      CritiqueTicket* ticket);
    SmartThinkingCandidate targeted_revision(
        const json& request,
        const SmartThinkingConfig& config,
        const MetaPlan& meta_plan,
        const SmartThinkingCandidate& candidate,
        const std::vector<CritiqueTicket>& tickets);
    SearchState generate_root_search_state(const json& request,
                                           const SmartThinkingConfig& config,
                                           const TaskProfile& profile);
    SearchState expand_search_state(const json& request,
                                    const SmartThinkingConfig& config,
                                    const SearchState& parent,
                                    int child_index,
                                    int target_depth,
                                    bool require_terminal,
                                    bool replacement_restart = false,
                                    const std::string& replacement_of = std::string{});
    ProcessVerdict verify_search_state(const json& request,
                                       const SmartThinkingConfig& config,
                                       SearchState* state,
                                       int verifier_index);
    RepairTicketVerdict confirm_repair_ticket(
        const json& request,
        const SmartThinkingConfig& config,
        const SearchState& candidate,
        const std::string& repair_ticket,
        int confirmation_index);
    SearchState repair_search_state(const json& request,
                                    const SmartThinkingConfig& config,
                                    const SearchState& root,
                                    const SearchState& candidate,
                                    const std::string& repair_ticket,
                                    const RepairTicketVerdict& confirmation,
                                    int repair_index);
    std::vector<SearchState> select_diverse_beam(
        const std::vector<SearchState>& states,
        int beam_width) const;
    ConsensusState compute_consensus(
        const std::vector<SmartThinkingCandidate>& candidates,
        const ComputePlan& plan,
        const TaskProfile& profile) const;
    SmartThinkingCandidate choose_best_candidate(
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus) const;
    json aggregate_candidates(const json& request,
                              const SmartThinkingConfig& config,
                              const MetaPlan& meta_plan,
                              const std::vector<SmartThinkingCandidate>& candidates,
                              const ConsensusState& consensus,
                              const std::vector<CritiqueTicket>& tickets);
    json audit_final_answer(const json& request,
                            const SmartThinkingConfig& config,
                            const std::string& proposed_final,
                            const std::vector<SmartThinkingCandidate>& candidates,
                            const ConsensusState& consensus,
                            bool* passed,
                            bool* correction_used,
                            std::string* failure);
    json run_single_pass(const json& request,
                         const SmartThinkingConfig& config,
                         const TaskProfile& profile);
    json run_conservative_deliberation(const json& request,
                                       const SmartThinkingConfig& config,
                                       const TaskProfile& profile);
    json run_fresh_context_search(const json& request,
                                  const SmartThinkingConfig& config,
                                  const TaskProfile& profile);
    std::optional<json> run_verified_capability(
        const json& request,
        const SmartThinkingConfig& config,
        const TaskProfile& profile,
        const SmartThinkingCapabilityRouteResult& route,
        const std::string& verified_task_text);
    json run_native_fallback_passthrough(
        const json& request,
        const SmartThinkingConfig& config,
        const TaskProfile& profile,
        const std::string& fallback_reason,
        bool preserve_request_exactly = false);
    json make_verified_required_error(
        const SmartThinkingCapabilityRouteResult& route) const;
    json make_verified_terminal_response(
        const json& request,
        const SmartThinkingConfig& config,
        const TaskProfile& profile,
        const std::string& final_text,
        const std::string& stop_reason,
        const std::string& verifier_summary,
        const std::string& response_id);
    json run_tool_request(const json& request,
                          const SmartThinkingConfig& config,
                          const TaskProfile& profile);
    json repair_final_answer(const json& request,
                             const SmartThinkingConfig& config,
                             const std::string& previous_text,
                             const std::string& validation_failure);
    json finalize_reasoning_only_response(const json& request,
                                          const SmartThinkingConfig& config,
                                          const json& source_response,
                                          const std::string& private_reasoning,
                                          std::string* failure);

    static ParsedArtifact parse_candidate_artifact(
        const json& response,
        const SmartThinkingOutputRequirements& requirements);
    static MetaPlan parse_meta_plan(const json& response);
    static ToolPlan parse_tool_plan(const json& response, int plan_index);
    static DisputeFrame parse_dispute_frame(const json& response);
    static BlindVerdict parse_blind_verdict(const json& response);
    static SearchState parse_search_state(
        const json& response,
        const SmartThinkingOutputRequirements& requirements,
        int id,
        int parent_id,
        int depth,
        int branch_index);
    static SearchState make_bootstrap_root_search_state(
        const std::string& recovery_failure);
    static RepairTicketVerdict parse_repair_ticket_verdict(const json& response);
    static ProcessVerdict parse_process_verdict(const json& response);
    static CritiqueReport parse_critique_report(
        const json& response,
        const std::vector<SmartThinkingCandidate>& candidates,
        const ConsensusState& consensus,
        int max_actionable_tickets);
    static std::string canonicalize_answer(
        const std::string& answer,
        const SmartThinkingOutputRequirements& requirements);
    static bool request_contains_active_tools(const json& request);

    void reset_runtime_state();
    json invoke_generator(const GenerateFn& generator,
                          const json& request,
                          std::string* failure = nullptr);
    void record_response_usage(const json& response);
    json apply_aggregated_usage(json response) const;
    json sanitize_final_response(json response,
                                 const SmartThinkingOutputRequirements& requirements,
                                 const std::string& fallback_text,
                                 std::string* validation_failure) const;
    json make_debug_metadata(const SmartThinkingConfig& config,
                             const TaskProfile& profile,
                             const ComputePlan& plan,
                             const ConsensusState& consensus,
                             const std::string& stop_reason,
                             const SmartThinkingCriticResult& result,
                             const std::string& validation_failure) const;

    SmartThinkingUsage usage_;
    bool aggregation_used_ = false;
    bool final_audit_used_ = false;
    bool final_audit_passed_ = false;
    bool final_audit_correction_used_ = false;
    bool repair_used_ = false;
    bool best_effort_returned_ = false;
    bool meta_plan_used_ = false;
    bool critique_used_ = false;
    bool targeted_revision_used_ = false;
    int generated_candidates_ = 0;
    int backend_failures_ = 0;
    int critique_ticket_count_ = 0;
    int actionable_ticket_count_ = 0;
    int confirmed_ticket_count_ = 0;
    int dispute_probe_count_ = 0;
    int targeted_revision_count_ = 0;
    int reasoning_finalization_attempts_ = 0;
    int reasoning_finalization_successes_ = 0;
    bool conservative_deliberation_used_ = false;
    bool challenger_generated_ = false;
    bool dispute_frame_used_ = false;
    bool dispute_frame_checkable_ = false;
    int blind_verification_count_ = 0;
    bool label_swap_consistent_ = false;
    bool switched_from_primary_ = false;
    std::string blind_verification_first_ = "not_run";
    std::string blind_verification_swapped_ = "not_run";
    bool tool_reasoning_used_ = false;
    int tool_plan_count_ = 0;
    bool tool_plan_agreement_ = false;
    std::string selected_tool_name_;
    int meta_plan_difficulty_ = 0;
    std::string sampling_stop_reason_ = "not_started";
    std::string judge_backend_ = "not_used";

    bool fresh_context_search_used_ = false;
    int search_states_generated_ = 0;
    int search_states_verified_ = 0;
    int search_states_pruned_ = 0;
    int search_depth_reached_ = 0;
    int search_final_candidate_count_ = 0;
    int search_repair_attempts_ = 0;
    int search_repair_candidates_ = 0;
    int search_repair_ticket_resolved_ = 0;
    int search_ticket_checks_ = 0;
    int search_tickets_confirmed_ = 0;
    int search_tickets_rejected_ = 0;
    int search_tickets_abstained_ = 0;
    int search_deduplicated_candidates_ = 0;
    int search_audit_reuses_ = 0;
    int search_synthetic_reuses_ = 0;
    int search_independent_candidates_ = 0;
    bool search_independent_agreement_ = false;
    int search_replacement_attempts_ = 0;
    int search_replacement_successes_ = 0;
    int search_trusted_roots_ = 0;
    int search_untrusted_roots_ = 0;
    int search_structural_gates_ = 0;
    int search_progressive_continuations_ = 0;
    int search_root_recovery_attempts_ = 0;
    int search_root_recovery_successes_ = 0;
    bool search_root_bootstrap_used_ = false;
    bool search_reference_used_ = false;
    bool search_reference_valid_ = false;
    std::string search_reference_answer_;
    std::string search_reference_failure_;
    int search_reference_matched_state_id_ = -1;
    int search_selected_state_id_ = -1;
    double search_selected_score_ = 0.0;
    std::string search_stop_reason_ = "not_started";

    bool verified_execution_attempted_ = false;
    bool verified_execution_used_ = false;
    std::string verified_execution_kernel_;
    std::string verified_contract_version_;
    std::string verified_detector_status_ = "not_attempted";
    std::string verified_detector_reason_;
    json verified_detector_diagnostics_ = json::object();
    json verified_capability_registry_ = json::array();
    std::string verified_compile_status_ = "not_attempted";
    std::string verified_compile_failure_;
    std::string verified_validation_status_ = "not_attempted";
    std::string verified_execution_status_ = "not_attempted";
    std::string verified_program_hash_;
    int verified_operation_count_ = 0;
    int verified_task_count_ = 0;
    int verified_chunk_size_ = 0;
    int verified_proposal_calls_ = 0;
    int verified_model_calls_ = 0;
    int verified_transition_attempts_ = 0;
    int verified_operations_executed_ = 0;
    int verified_completion_events_ = 0;
    int verified_tasks_completed_ = 0;
    int verified_search_nodes_ = 0;
    int verified_claims_ = 0;
    int verified_checks_ = 0;
    int verified_verification_events_ = 0;
    json verified_verification_ledger_ = json::object();
    std::int64_t verified_execution_time_ms_ = 0;
    bool fallback_request_equivalent_ = false;
    bool fallback_request_budget_adjusted_ = false;
    std::string fallback_budget_policy_ = "not_used";
    std::string fallback_token_field_;
    int fallback_original_token_limit_ = 0;
    int fallback_effective_token_limit_ = 0;
    json fallback_request_changes_ = json::array();
    std::string fallback_request_hash_;
    std::string fallback_reason_;
    int fallback_model_calls_ = 0;
    std::string verified_stop_reason_ = "not_started";

    json search_debug_candidates_ = json::array();
    json search_debug_trace_ = json::array();
    SmartThinkingSearchEventLog search_event_log_;
};

const char* to_string(SmartThinkingMode mode);
const char* to_string(SmartThinkingCritic critic);
const char* to_string(SmartThinkingCloudAssist cloud_assist);
const char* to_string(SmartThinkingToolPolicy tool_policy);
const char* to_string(SmartThinkingSelectionPolicy policy);
const char* to_string(SmartThinkingExecutionPolicy policy);
const char* to_string(SmartThinkingProductTier tier);
const char* to_string(SmartThinkingBackendBatching batching);

}  // namespace lemon
