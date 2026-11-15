#pragma once

// Pure, inexpensive routing preparation. Keep the action vocabulary separate
// from the role prompts: the zero-shot model sees a small positive label menu,
// while unambiguous requested deliverables do not depend on a noisy classifier.
// No model calls, session state or extra prompt injection in this header.
#include "lemon/server/smart_roles.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <cmath>
#include <vector>

namespace lemon::smart_roles {

struct ActionCue {
    std::string_view role;
    std::string_view phrase;
    int weight;            // retrieval evidence, not a probability
    bool explicit_output; // high precision action+deliverable, not just a topic
};

// The cues are deliberately few and action-oriented. They shortlist *methods*,
// never change the 14 role definitions. Unrecognized languages/phrases fall back
// to the full label menu instead of silently suppressing a correct role.
inline constexpr ActionCue kActionCues[] = {
    {"plan", "help with building", 3, false},
    {"plan", "help building", 3, false},
    {"plan", "how to build", 3, false},
    {"plan", "make a pr", 3, false},
    {"plan", "create a pull request", 3, false},
    {"plan", "create a pr", 3, false},
    {"plan", "write a pr", 3, false},
    {"plan", "i want to build", 3, false},
    {"plan", "i want to create", 2, false},
    {"plan", "ich möchte einen pr", 3, false},
    {"plan", "prepare a pr", 3, false},
    {"plan", "plan the", 4, true},
    {"plan", "step by step", 2, false},
    {"plan", "what are the steps", 3, false},
    {"plan", "what should i do", 3, false},
    {"plan", "where should i start", 3, false},
    {"plan", "roadmap", 1, false},
    {"plan", "wie gehe ich vor", 3, false},
    {"plan", "ich moechte ein projekt", 2, false},
    {"plan", "ich möchte ein projekt", 2, false},
    {"plan", "bau eines", 2, false},
    {"code", "provide me with the code", 5, true},
    {"code", "provide me with code", 5, true},
    {"code", "give me source code", 5, true},
    {"code", "give me the code", 5, true},
    {"code", "write the code", 5, true},
    {"code", "write code", 4, true},
    {"code", "write a function", 5, true},
    {"code", "write a python script", 5, true},
    {"code", "write a bash script", 5, true},
    {"code", "write a shell script", 5, true},
    {"code", "write a python", 5, true},
    {"code", "need a patch", 4, true},
    {"code", "provide the implementation", 5, true},
    {"code", "implement this", 5, true},
    {"code", "implement the", 5, true},
    {"code", "implementiere", 5, true},
    {"code", "liefere den code", 5, true},
    {"code", "schreibe code", 5, true},
    {"code", "provide a patch", 5, true},
    {"code", "liefere einen patch", 5, true},
    {"code", "add unit tests", 3, true},
    {"code_review", "review this pr", 5, true},
    {"code_review", "review the pr", 5, true},
    {"code_review", "review this pull request", 5, true},
    {"code_review", "review the code", 5, true},
    {"code_review", "review the implementation", 5, true},
    {"code_review", "review the changes", 5, true},
    {"code_review", "review this patch", 5, true},
    {"code_review", "code review", 1, false},
    {"code_review", "pr review", 1, false},
    {"code_review", "prüfe den pr", 5, true},
    {"code_review", "pruefe den pr", 5, true},
    {"code_review", "reviewe den pr", 5, true},
    {"code_review", "prüfe den code", 5, true},
    {"write", "write the pr description", 5, true},
    {"write", "write a pr description", 5, true},
    {"write", "write the pull request description", 5, true},
    {"write", "draft the pr description", 5, true},
    {"write", "draft a pr description", 5, true},
    {"write", "write a commit message", 5, true},
    {"write", "draft an email", 5, true},
    {"write", "write an email", 5, true},
    {"write", "write a report", 5, true},
    {"write", "write a response", 4, true},
    {"write", "pr-beschreibung", 1, false},
    {"write", "pr beschreibung", 1, false},
    {"write", "formuliere eine e-mail", 5, true},
    {"write", "schreibe eine mail", 5, true},
    {"write", "commit-message", 1, false},
    {"creative_write", "write a story", 5, true},
    {"creative_write", "write a fantasy", 5, true},
    {"creative_write", "write a poem", 5, true},
    {"creative_write", "write a song", 5, true},
    {"creative_write", "fantasy story", 1, false},
    {"creative_write", "schreib eine geschichte", 5, true},
    {"creative_write", "schreib ein gedicht", 5, true},
    {"transform", "translate this", 5, true},
    {"transform", "summarize this", 5, true},
    {"transform", "translate the", 5, true},
    {"transform", "summarize the", 5, true},
    {"transform", "übersetze", 5, true},
    {"transform", "uebersetze", 5, true},
    {"transform", "fasse zusammen", 5, true},
    {"transform", "rephrase this", 5, true},
    {"explain", "explain how", 5, true},
    {"explain", "explain why", 5, true},
    {"explain", "explain this", 5, true},
    {"explain", "what is", 3, false},
    {"explain", "what does", 4, false},
    {"explain", "was bedeutet", 4, false},
    {"explain", "how does", 3, false},
    {"explain", "erkläre", 5, true},
    {"explain", "erklaere", 5, true},
    {"advise", "should i choose", 4, false},
    {"advise", "which should i choose", 4, false},
    {"advise", "compare the options", 4, true},
    {"advise", "which material", 3, false},
    {"advise", "was empfiehlst", 4, false},
    {"advise", "welche variante", 3, false},
    {"explore", "brainstorm", 5, true},
    {"explore", "give me ideas", 5, true},
    {"explore", "different ideas", 3, false},
    {"explore", "sammle ideen", 5, true},
    {"solve", "solve the equation", 5, true},
    {"solve", "calculate", 4, true},
    {"solve", "berechne", 4, true},
    {"solve", "beweise", 4, true},
    {"diagnose", "debug this", 5, true},
    {"diagnose", "root cause", 4, false},
    {"diagnose", "why does it crash", 4, false},
    {"diagnose", "traceback", 1, false},
    {"diagnose", "find the bug", 5, true},
    {"diagnose", "warum stürzt", 4, false},
    {"investigate", "research the", 4, true},
    {"investigate", "verify the sources", 4, true},
    {"investigate", "find sources", 4, true},
    {"investigate", "recherchiere", 4, true},
    {"audit", "reviewing this design", 3, false},
    {"audit", "review the design", 4, true},
    {"audit", "critique the proposal", 5, true},
    {"audit", "audit this design", 5, true},
    {"audit", "find flaws in", 4, true},
    {"audit", "finde widersprüche", 4, true},
};

inline std::string routing_lower(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        // Minimal UTF-8 German case folding; all other bytes are preserved.
        if (i + 1 < input.size() && c == 0xC3) {
            const unsigned char next = static_cast<unsigned char>(input[i + 1]);
            if (next == 0x84 || next == 0x96 || next == 0x9C) {
                out.push_back(static_cast<char>(0xC3));
                out.push_back(static_cast<char>(next + 0x20));
                ++i;
                continue;
            }
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool routing_word_byte(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c >= 128;
}

inline bool routing_phrase(std::string_view input, std::string_view phrase) {
    size_t at = input.find(phrase);
    while (at != std::string_view::npos) {
        const size_t end = at + phrase.size();
        const bool left = at == 0 || !routing_word_byte(static_cast<unsigned char>(input[at - 1]));
        const bool right = end == input.size() || !routing_word_byte(static_cast<unsigned char>(input[end]));
        if (left && right) return true;
        at = input.find(phrase, at + 1);
    }
    return false;
}

inline bool routing_quoted(std::string_view input, std::string_view phrase) {
    const size_t at = input.find(phrase);
    if (at == std::string_view::npos) return false;
    const size_t line_start = input.rfind('\n', at);
    const size_t start = line_start == std::string_view::npos ? 0 : line_start + 1;
    const size_t end = input.find('\n', at);
    const auto line = input.substr(start, (end == std::string_view::npos ? input.size() : end) - start);
    const size_t pos = at - start;
    for (char quote : {'"', '\''}) {
        size_t before = 0;
        size_t after = 0;
        for (size_t i = 0; i < pos; ++i) if (line[i] == quote) ++before;
        for (size_t i = pos + phrase.size(); i < line.size(); ++i) if (line[i] == quote) ++after;
        if ((before & 1) && after) return true;
    }
    return false;
}

struct TaskSignal {
    std::array<int, kRoles.size()> scores{};
    std::string direct_role; // narrow explicit requested output, or empty
    bool refers_to_previous = false;
    bool temporary = false;
};

inline size_t role_index(std::string_view id) {
    for (size_t i = 0; i < kRoles.size(); ++i) if (kRoles[i].id == id) return i;
    return kRoles.size();
}

inline TaskSignal task_signal(std::string_view current, bool has_previous) {
    TaskSignal out;
    const std::string text = routing_lower(current);
    const auto starts_with = [&text](std::string_view prefix) { return text.rfind(prefix, 0) == 0; };
    // A quoted directive or a question *about* a phrase is not a directive.
    const bool metalinguistic = starts_with("what does ") || starts_with("what is the meaning") ||
        starts_with("what do the words") || starts_with("was bedeutet ") ||
        starts_with("erkläre den ausdruck") || starts_with("explain the phrase") ||
        starts_with("please explain the phrase");
    // A cue is a direct user request only when the message has an instruction
    // voice. Text that merely reports a request in a ticket/log is evidence
    // for the classifier, not grounds to skip it or switch roles immediately.
    const bool request_voice = starts_with("please ") || starts_with("can you ") ||
        starts_with("could you ") || starts_with("would you ") ||
        starts_with("i need ") || starts_with("i want ") ||
        starts_with("you need ") || starts_with("you must ") ||
        starts_with("du musst ") ||
        starts_with("help me ") || starts_with("write ") ||
        starts_with("now ") || starts_with("and now ") ||
        starts_with("implement ") || starts_with("review ") ||
        starts_with("give me ") || starts_with("provide ") ||
        starts_with("draft ") || starts_with("explain ") ||
        starts_with("translate ") || starts_with("summarize ") ||
        starts_with("brainstorm ") || starts_with("debug ") ||
        starts_with("research ") || starts_with("calculate ") ||
        starts_with("plan ") || starts_with("design ") || starts_with("bitte ") ||
        starts_with("kannst du ") || starts_with("ich brauche ") ||
        starts_with("ich möchte ") || starts_with("ich moechte ") ||
        starts_with("liefere ") || starts_with("schreibe ") ||
        starts_with("schreib ") || starts_with("prüfe ") ||
        starts_with("pruefe ") || starts_with("erkläre ") ||
        starts_with("erklaere ") || starts_with("übersetze ") ||
        starts_with("uebersetze ") || starts_with("berechne ") ||
        starts_with("formuliere ") || starts_with("recherchiere ") ||
        starts_with("fasse ") ||
        // A natural follow-up may describe the context first, then address
        // the model explicitly: '... and you have to provide me with the code'.
        (text.size() < 220 && text.find("you have to ") != std::string::npos &&
         !starts_with("the logs ") && !starts_with("the ticket ") &&
         !starts_with("he said ") && !starts_with("she said "));
    std::array<int, kRoles.size()> direct{};
    for (const auto& cue : kActionCues) {
        if (!routing_phrase(text, cue.phrase)) continue;
        const auto i = role_index(cue.role);
        if (i >= kRoles.size()) continue;
        out.scores[i] = std::max(out.scores[i], cue.weight);
        if (cue.explicit_output && request_voice && !metalinguistic &&
            !routing_quoted(text, cue.phrase))
            direct[i] = std::max(direct[i], cue.weight);
    }
    // Match action AND deliverable, not an isolated topic word. This covers
    // natural paraphrases beyond the exact idioms above ("provide code",
    // "einen Patch schreiben", "PR-Beschreibung erstellen") without turning
    // a generic mention of software or a PR into a coding instruction.
    const auto has = [&text](std::initializer_list<std::string_view> words) {
        for (const auto word : words) if (routing_phrase(text, word)) return true;
        return false;
    };
    const bool source_artifact = has({"code", "source code", "quellcode", "patch", "diff",
        "function", "funktion", "python script", "bash script", "shell script",
        "javascript script", "python skript", "source files", "tests"});
    const bool prose_artifact = has({"pr description", "pr-beschreibung", "pr beschreibung",
        "pull request description", "commit message", "commit-message", "email", "e-mail",
        "report", "letter", "article", "text", "nachricht", "beschreibung", "absatz",
        "code of conduct"});
    const bool story_artifact = has({"story", "poem", "song", "gedicht", "geschichte", "märchen"});
    const bool writing_action = has({"write", "draft", "compose", "formulate", "schreibe",
        "schreib", "schreiben", "formuliere", "erstelle", "erstellen", "design"});
    const bool code_action = has({"provide", "give", "deliver", "write", "generate",
        "implement", "create", "fix", "schreiben", "schreibe", "erstelle",
        "liefere", "liefern", "gib", "implementiere", "programmiere"});
    const bool review_action = has({"review", "inspect", "check", "prüfe", "pruefe",
        "reviewe", "überprüfe", "kontrolliere"});
    const bool transform_action = has({"summarize", "translate", "extract", "rephrase",
        "übersetze", "uebersetze", "übersetzen", "fasse zusammen", "zusammenfassen"});
    const bool diagnose_action = has({"debug", "troubleshoot", "find the root cause",
        "diagnose", "analysiere den fehler", "untersuche den fehler"});
    if (request_voice && !metalinguistic) {
        if (transform_action && !writing_action) {
            direct[role_index("code_review")] = 0;
            direct[role_index("transform")] = 5;
        } else if (diagnose_action && !writing_action) {
            direct[role_index("code")] = 0;
            direct[role_index("diagnose")] = 5;
        } else if (review_action && prose_artifact && !source_artifact) {
            direct[role_index("code_review")] = 0;
            direct[role_index("transform")] = 5;
        } else if (writing_action && story_artifact && !prose_artifact && !source_artifact) {
            direct[role_index("creative_write")] = 5;
        } else if (writing_action && prose_artifact && !source_artifact) {
            direct[role_index("code")] = 0;
            direct[role_index("code_review")] = 0;
            direct[role_index("write")] = 5;
        } else if (source_artifact && code_action && !prose_artifact) {
            direct[role_index("code")] = 5;
        } else if (review_action && has({"pr", "pull request", "code", "patch", "diff"}) &&
                   !prose_artifact) {
            direct[role_index("code_review")] = 5;
        } else if (writing_action && has({"plan", "roadmap", "einen plan"}) &&
                   !prose_artifact && !source_artifact) {
            direct[role_index("plan")] = 5;
        }
    }
    // An isolated report or quotation about a request must not shrink the
    // classifier menu toward an unrelated method just because it mentions it.
    const bool question_voice = starts_with("how ") || starts_with("what ") ||
        starts_with("why ") || starts_with("which ") || starts_with("wie ") ||
        starts_with("was ") || starts_with("warum ") || starts_with("welche ") ||
        routing_phrase(text, "what should i do") || routing_phrase(text, "wie gehe ich vor");
    if (!request_voice && !question_voice) out.scores.fill(0);

    // Learning *about* a requested deliverable is not requesting that output.
    // Do not let phrases from the object of an explanation override the user's
    // actual instruction ("Explain how to write code" -> explain).
    const bool teaching_request = starts_with("how do i ") || starts_with("how can i ") ||
        starts_with("how should i ") || starts_with("wie kann ich ") ||
        starts_with("wie soll ich ") || starts_with("i want to learn ") ||
        starts_with("i need to understand ") || starts_with("please explain ") ||
        starts_with("can you explain ") || starts_with("teach me ") ||
        starts_with("would you teach ") || starts_with("ich möchte lernen ") ||
        starts_with("ich moechte lernen ") || starts_with("bitte erkläre ");
    if (teaching_request) {
        for (size_t i = 1; i < direct.size(); ++i)
            if (kRoles[i].id != "explain") direct[i] = 0;
        out.scores[role_index("explain")] = std::max(out.scores[role_index("explain")], 3);
    }
    // Do not use a direct cue if multiple different outputs are being requested.
    int best = 0;
    size_t winner = kRoles.size();
    bool tied = false;
    for (size_t i = 1; i < direct.size(); ++i) {
        if (direct[i] > best) { best = direct[i]; winner = i; tied = false; }
        else if (direct[i] > 0 && direct[i] == best) tied = true;
    }
    if (winner < kRoles.size() && best >= 4 && !tied) out.direct_role = std::string(kRoles[winner].id);
    out.refers_to_previous = has_previous && text.size() < 240 &&
        (routing_phrase(text, "it") || routing_phrase(text, "this") ||
         routing_phrase(text, "that") || routing_phrase(text, "continue") ||
         routing_phrase(text, "do that") || routing_phrase(text, "now") ||
         routing_phrase(text, "das") || routing_phrase(text, "es") ||
         routing_phrase(text, "dazu") || routing_phrase(text, "dafür") ||
         routing_phrase(text, "weiter") || routing_phrase(text, "nochmal") ||
         routing_phrase(text, "yes") || routing_phrase(text, "ja") ||
         routing_phrase(text, "go ahead") || routing_phrase(text, "mach weiter"));
    out.temporary = has_previous &&
        (out.direct_role == "write" || out.direct_role == "transform" ||
         out.direct_role == "explain") &&
        text.size() < 180;
    return out;
}

inline std::string_view classifier_description(std::string_view id) {
    for (const auto& entry : std::array<std::pair<std::string_view, std::string_view>, 14>{{
        {"default", "Casual conversation, greetings, or no identifiable task"},
        {"explore", "Brainstorm different ideas and possibilities"},
        {"plan", "Work out steps and dependencies to reach a practical goal or start a project"},
        {"solve", "Solve a math, logic, or technical problem with a verifiable result"},
        {"investigate", "Find evidence and verify claims using sources"},
        {"diagnose", "Troubleshoot an error or investigate the cause of a failure"},
        {"code", "Implement working software, source code, a patch, or tests"},
        {"code_review", "Inspect existing code or a pull request for bugs and regressions"},
        {"audit", "Challenge a design or argument and identify flaws"},
        {"explain", "Teach a concept and explain how or why it works"},
        {"advise", "Compare alternatives and recommend a choice"},
        {"write", "Compose finished prose such as an email, article, or PR description"},
        {"creative_write", "Create an original story, poem, or fictional scene"},
        {"transform", "Translate, summarize, or faithfully reshape given material"},
    }}) if (entry.first == id) return entry.second;
    return {};
}

inline std::vector<size_t> classifier_menu(const TaskSignal& signal, std::string_view active) {
    int max_score = 0;
    for (int value : signal.scores) max_score = std::max(max_score, value);
    std::vector<size_t> ids;
    if (max_score < 2) {
        for (size_t i = 0; i < kRoles.size(); ++i) ids.push_back(i);
        return ids;
    }
    auto add = [&ids](size_t id) {
        if (id < kRoles.size() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    add(0); // Default is always available.
    // Include every independently supported method, then near methods. This
    // avoids a 'PR' topic noun directly adding the prose-writing role.
    for (size_t i = 1; i < kRoles.size(); ++i)
        if (signal.scores[i] >= 2) add(i);
    size_t focused = 0;
    for (size_t i = 1; i < signal.scores.size(); ++i)
        if (signal.scores[i] > signal.scores[focused]) focused = i;
    if (focused != 0) {
        // Keep close neighboring methods available for the model to reject
        // an overly broad lexical cue. Unlike the old 213-item catalog this
        // is a small static neighborhood, not an additional routing pass.
        const auto id = kRoles[focused].id;
        if (id == "plan") {
            add(role_index("explore")); add(role_index("advise")); add(role_index("explain"));
        } else if (id == "code") {
            add(role_index("diagnose")); add(role_index("code_review")); add(role_index("plan"));
        } else if (id == "write") {
            add(role_index("transform")); add(role_index("creative_write")); add(role_index("plan"));
        } else if (id == "code_review") {
            add(role_index("diagnose")); add(role_index("audit")); add(role_index("code"));
        } else if (id == "explore") {
            add(role_index("plan")); add(role_index("creative_write"));
        } else if (id == "diagnose") {
            add(role_index("code")); add(role_index("solve"));
        } else if (id == "solve") {
            add(role_index("explain")); add(role_index("diagnose"));
        } else if (id == "investigate") {
            add(role_index("audit")); add(role_index("explain"));
        } else if (id == "audit") {
            add(role_index("investigate")); add(role_index("code_review"));
        } else if (id == "explain") {
            add(role_index("solve")); add(role_index("advise"));
        } else if (id == "advise") {
            add(role_index("plan")); add(role_index("explore"));
        } else if (id == "creative_write") {
            add(role_index("explore")); add(role_index("write"));
        } else if (id == "transform") {
            add(role_index("write")); add(role_index("explain"));
        }
    }
    if (signal.refers_to_previous) add(role_index(active)); // continuation only
    // Do not exceed eight labels; excess cues indicate a compound task, so
    // offer all fourteen rather than silently drop a supported candidate.
    if (ids.size() > 8) {
        ids.clear();
        for (size_t i = 0; i < kRoles.size(); ++i) ids.push_back(i);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline Decision direct_decision(const TaskSignal& signal) {
    Decision d;
    if (signal.direct_role.empty() || !find(signal.direct_role)) {
        d.valid = false;
        return d;
    }
    // Rules are binary evidence about an explicit deliverable, not classifier
    // probabilities. Report provenance separately from model scores in logs.
    d.first = {signal.direct_role, 1.0};
    d.second = {"default", 0.0};
    d.intent = Intent::Explicit;
    d.scope = signal.temporary ? Scope::Subtask : Scope::NewTask;
    return d;
}

inline Decision score_decision(const std::vector<double>& raw, const std::vector<size_t>& indices,
                               const TaskSignal& signal) {
    Decision d;
    if (raw.size() != indices.size() || raw.size() < 2 || raw.size() > kRoles.size() ||
        std::find(indices.begin(), indices.end(), 0) == indices.end()) {
        d.valid = false;
        return d;
    }
    std::array<bool, kRoles.size()> seen{};
    double total = 0;
    std::vector<Candidate> ranked;
    for (size_t i = 0; i < raw.size(); ++i) {
        const size_t index = indices[i];
        if (index >= kRoles.size() || seen[index] || !std::isfinite(raw[i]) ||
            raw[i] < 0 || raw[i] > 1) {
            d.valid = false;
            return d;
        }
        seen[index] = true;
        total += raw[i];
        ranked.push_back({std::string(kRoles[index].id), raw[i]});
    }
    if (total < 1e-6 || !std::isfinite(total)) {
        d.valid = false;
        return d;
    }
    std::sort(ranked.begin(), ranked.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    // Normalise over *all* offered labels, not just the first two. Scores
    // remain relative evidence, not calibrated confidence percentages.
    d.first = {ranked[0].id, ranked[0].score / total};
    d.second = {ranked[1].id, ranked[1].score / total};
    d.intent = Intent::Implicit;
    d.scope = signal.refers_to_previous ? Scope::Continuation : Scope::NewTask;
    const size_t top_index = role_index(d.first.id);
    d.task_supported = top_index < kRoles.size() && signal.scores[top_index] >= 3;
    return d;
}

inline std::string classifier_input(std::string_view current, std::string_view previous,
                                    const TaskSignal& signal) {
    std::string input;
    // Previous intent is ONLY for short follow-ups. Put it first so the current
    // instruction stays most salient and is not truncated from the tail.
    if (signal.refers_to_previous && !previous.empty())
        input = "Previous user goal (background only): " + std::string(previous.substr(0, 350)) + "\n";
    input += "Current user request (classify the work needed NOW): " + std::string(current.substr(0, 1200));
    return input;
}

} // namespace lemon::smart_roles
