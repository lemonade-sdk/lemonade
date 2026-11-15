#pragma once

// Experimental, backend-independent role transition logic. This header does not
// execute an inference call or persist session data: its caller owns the state.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace lemon::smart_roles {

struct Role {
    std::string_view id;
    std::string_view family;
    bool strong;
    std::string_view description;
    std::string_view prompt;
};

inline constexpr std::array<Role, 14> kRoles = {{
    {"default", "default", false, "Casual conversation or unclear task with no identifiable requested method; use only if no specialist fits", ""},
    {"explore", "ideas", false, "Brainstorm different possible approaches or ideas before committing to a plan or an outcome", "Explore a wide but relevant range of approaches. Identify fundamentally different options, their likely strengths, assumptions and trade-offs. Distinguish fresh ideas from proven solutions. Do not prematurely choose a single implementation or turn brainstorming into a rigid plan. Respect any constraints already provided and avoid padding the answer with merely cosmetic variants."},
    {"plan", "ideas", false, "Plan how to achieve a concrete goal or start a project: practical steps, order and dependencies; not finished text or code", "Start from the intended outcome and explicit requirements. Organize work into ordered, feasible steps with dependencies, acceptance criteria, tests and rollback where relevant. Identify risks that could invalidate the approach and the smallest checks to resolve them. Prefer reviewable milestones and minimal scope over speculative refactors. Do not claim work has been performed when producing a plan."},
    {"solve", "reason", true, "Determine the correct solution to a mathematical, logical or technical question, not merely explain it", "Identify the objective, assumptions, available inputs, relevant constraints and units. Derive a result systematically and test important steps against edge cases or independent checks. Distinguish a verified answer from an estimate. If information is insufficient, explain precisely what is missing rather than guessing. Present enough of the method for the user to evaluate the conclusion."},
    {"investigate", "research", true, "Find new factual evidence, check sources or verify claims using available research capabilities", "Determine which claims need new evidence and prioritize directly relevant, authoritative and recent sources where available. Trace conclusions back to what was actually checked. Distinguish primary evidence, secondary reporting, inference and unresolved uncertainty. Do not pretend to have used tools or accessed sources that are unavailable. Give specific, verifiable conclusions and avoid turning the task into unsupported speculation."},
    {"diagnose", "software", true, "Diagnose causes of faults, broken behavior, error messages or unexpected results", "Separate observed symptoms and logs from hypothetical causes. Trace the failure to the earliest plausible causal point and rank competing explanations. Suggest small discriminating tests before modifying unrelated systems. Account for reproduction conditions, error propagation, environment and recent changes. Recommend the smallest supported correction and clearly distinguish proven causes from untested theories."},
    {"code", "software", true, "Implement, modify or fix actual software code and tests; not plan a feature or prepare a PR", "Preserve the architecture, contracts and conventions of the existing codebase. Implement the smallest complete change that satisfies the user request; inspect callers, error paths, concurrency and backwards compatibility. Add focused tests for changed behavior and relevant regressions. Do not introduce parallel frameworks, speculative abstractions or unrelated refactors. State exactly which files were changed and which checks actually ran."},
    {"code_review", "software", true, "Critically inspect existing code or a PR diff for real bugs and regressions; not prepare a new PR", "Inspect the actual diff together with surrounding callers and assumptions. Seek reproducible correctness bugs, security flaws, compatibility regressions, concurrency hazards and test gaps. For each actionable finding, identify its trigger, impact and evidence; distinguish confirmed defects from hypotheses. Prioritize serious issues and minimize false positives. Do not treat style preferences or speculative improvements as blocking findings or silently rewrite the code."},
    {"audit", "reason", true, "Critique a design, proposal or reasoning for flaws, unsupported claims and counterexamples", "Challenge key claims, assumptions, boundaries, invariants and dependencies. Look for consequential contradictions, counterexamples, unsupported assertions and neglected failure scenarios. Rank genuine risks by likelihood and impact, and offer concrete validation or mitigation steps. Distinguish facts, uncertain concerns and stylistic preferences. Do not invent defects or demand a complete redesign when a focused correction suffices."},
    {"explain", "reason", false, "Teach or clarify how something works or what it means, rather than plan a project", "Begin with the central principle in language appropriate to the user context. Build explanations progressively from essential facts to relevant mechanisms, examples or counterexamples. State simplifications and meaningful exceptions explicitly. Avoid unnecessary detail, tangents or performative complexity. An explanation is not an excuse to start changing code or to fabricate current research; answer the question that was actually asked."},
    {"advise", "decision", false, "Compare alternatives against criteria and recommend a choice; not a sequential action plan", "Identify the intended decision and the real criteria: goals, constraints, failure risks, costs and reversibility. Compare viable options against those criteria rather than against generic feature lists. Make a proportionate recommendation when the evidence supports one, and indicate what new information could change it. Avoid false equivalence, invented measurements or unrelated shopping suggestions. Respect the user’s stated preferences and decision authority."},
    {"write", "writing", false, "Produce the finished requested text, e.g. a message, email, report or PR description; not organize the activity itself", "Deliver copy suitable for the requested recipient, channel, purpose, length and tone. Preserve supplied facts, names, claims and technical details accurately; do not invent approvals, tests or completed work. Structure the result for clarity and intended action. Keep the final text directly reusable without irrelevant commentary or broad planning. Do not turn professional writing into fiction unless explicitly requested."},
    {"creative_write", "creative", true, "Create a finished story, poem or other imaginative literary work, not general brainstorming", "Create an original piece grounded in the requested setting, characters, genre, voice and constraints. Use specific evocative details that advance the work rather than generic flourishes. Keep motivations, point of view, world rules and sequence of events consistent. Favor meaningful narrative development and style appropriate to the requested audience. Avoid unnecessary meta commentary and do not confuse fictional claims with factual statements."},
    {"transform", "writing", false, "Translate, summarize, extract or reformat existing material faithfully; not compose new content", "Treat the source material as authoritative and preserve its meaning, important facts, uncertainty, quantities, names and relevant tone. Perform only the requested transformation, such as translation, summary, extraction or reformatting. Do not inject new claims or silently alter conclusions. Preserve enough information for the user’s intended use while following formatting constraints precisely. If the source is ambiguous, do not manufacture certainty."},
}};

inline const Role* find(std::string_view id) {
    for (const auto& role : kRoles) if (role.id == id) return &role;
    return nullptr;
}

struct State {
    std::string active = "default";
    std::array<double, kRoles.size()> evidence{};
};

struct Candidate {
    std::string id = "default";
    double score = 0;
};

enum class Intent { None, Implicit, Explicit };
enum class Scope { Continuation, Subtask, NewTask, Unclear };

struct Decision {
    Candidate first{};
    Candidate second{};
    Intent intent = Intent::None;
    Scope scope = Scope::Unclear;
    bool valid = true;
    bool task_supported = false; // independent, non-model action evidence
};

struct Result {
    std::string effective = "default";
    State next{};
    bool temporary = false;
};

inline double cost(std::string_view from, std::string_view to) {
    if (from == to || to == "default") return 0;
    if (from == "default") return 0.5;
    if ((from == "code" && (to == "diagnose" || to == "code_review")) ||
        (from == "explore" && to == "plan") || (from == "write" && to == "transform")) return 1;
    if (from == "code_review" && to == "code") return 2;
    const auto* a = find(from);
    const auto* b = find(to);
    if (!a || !b) return 4;
    return a->family == b->family ? 1.5 : 4;
}

inline Result transition(const State& prior, const Decision& d,
                         std::string_view override_role = {}) {
    Result out;
    out.next = prior;
    if (!find(out.next.active)) out.next = {};
    if (!override_role.empty()) {
        if (find(override_role)) out.effective = std::string(override_role);
        // A manual role is a per-request override; it must not corrupt AUTO state.
        else out.effective = "default";
        return out;
    }

    // Bounded exponential forgetting; contradictory evidence cannot accumulate
    // indefinitely. A genuine new task discards stale intent evidence.
    if (d.scope == Scope::NewTask) out.next.evidence.fill(0);
    else for (double& value : out.next.evidence) value *= 0.65;
    const Role* first = find(d.first.id);
    const Role* second = find(d.second.id);
    const bool scores_ok = std::isfinite(d.first.score) && std::isfinite(d.second.score) &&
        d.first.score >= 0 && d.first.score <= 1 && d.second.score >= 0 && d.second.score <= 1 &&
        d.first.score >= d.second.score && (!second || first != second);
    if (!d.valid || !scores_ok || !first) {
        out.effective = d.scope == Scope::Continuation ? out.next.active : "default";
        return out;
    }
    const double margin = d.first.score - d.second.score;
    const bool confident = d.first.score >= 0.60 && margin >= 0.15;
    const auto index = static_cast<size_t>(first - kRoles.data());
    if (first->id != "default" && confident) {
        out.next.evidence[index] = std::min(3.0, out.next.evidence[index] + d.first.score + margin);
    }
    if (first->id == "default") {
        out.effective = d.scope == Scope::Continuation ? out.next.active : "default";
        // A clearly separate conversation turn must not leave a stale role
        // armed for a later follow-up after a high-confidence default result.
        if (d.scope == Scope::NewTask && confident) out.next.active = "default";
        return out;
    }
    const bool independent_task = d.scope == Scope::NewTask && confident &&
        (out.next.active == "default" ? d.first.score >= 0.60 && margin >= 0.20
                                       : d.first.score >= 0.75 && margin >= 0.30);
    const bool supported_task = d.task_supported && d.scope == Scope::NewTask &&
        (out.next.active == "default" ? d.first.score >= 0.34 && margin >= 0.10
                                       : d.first.score >= 0.53 && margin >= 0.18);
    const bool immediate = d.intent == Intent::Explicit && confident;
    const double threshold = 0.9 + 0.35 * cost(out.next.active, first->id) +
                             (first->strong ? 0.2 : 0.0);
    const bool gradual = confident && out.next.evidence[index] >= threshold && margin >= 0.20;
    if (immediate || independent_task || supported_task || gradual) {
        out.effective = std::string(first->id);
        if (d.scope == Scope::Subtask) out.temporary = true;
        else out.next.active = out.effective;
    } else {
        out.effective = d.scope == Scope::Continuation ? out.next.active : "default";
    }
    return out;
}

} // namespace lemon::smart_roles
