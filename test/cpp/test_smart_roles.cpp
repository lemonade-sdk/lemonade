#include "lemon/server/smart_roles_selector.h"
// Keep all assertions active even in Release CI configurations.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <vector>
using namespace lemon::smart_roles;

static bool offered(const std::vector<size_t>& menu, std::string_view role) {
    return std::find(menu.begin(), menu.end(), role_index(role)) != menu.end();
}

static void assert_direct(const std::string& message, const char* expected, bool prior = false) {
    const TaskSignal signal = task_signal(message, prior);
    if (signal.direct_role != expected) {
        std::cerr << "expected direct " << expected << ", got " << signal.direct_role
                  << " for: " << message << '\n';
        assert(false);
    }
}

int main() {
    static_assert(kRoles.size() == 14);
    assert(find("default") && find("default")->prompt.empty());
    assert(cost("code", "code_review") < cost("code", "creative_write"));
    assert(cost("code", "code_review") != cost("code_review", "code"));
    State state;
    state.active = "code";
    Decision review{{"code_review", .86}, {"code", .12}, Intent::Explicit, Scope::NewTask};
    auto result = transition(state, review);
    assert(result.effective == "code_review");
    assert(result.next.active == "code_review");
    Decision story{{"creative_write", .92}, {"default", .03}, Intent::Explicit, Scope::NewTask};
    result = transition(state, story);
    assert(result.effective == "creative_write");
    Decision vague{{"creative_write", .45}, {"code", .42}, Intent::Implicit, Scope::Continuation};
    result = transition(state, vague);
    assert(result.effective == "code");
    Decision subtask{{"write", .95}, {"default", .01}, Intent::Explicit, Scope::Subtask};
    result = transition(state, subtask);
    assert(result.effective == "write" && result.temporary && result.next.active == "code");
    result = transition(state, subtask, "explain");
    assert(result.effective == "explain" && result.next.active == "code");
    result = transition(state, Decision{{"creative_write", 1.2}, {"default", 0}, Intent::Explicit, Scope::NewTask});
    assert(result.effective == "default");
    result = transition(state, Decision{{"unknown", .99}, {"default", .01}, Intent::Explicit, Scope::NewTask});
    assert(result.effective == "default");
    auto implicit = Decision{{"code_review", .92}, {"code", .03}, Intent::Implicit, Scope::Continuation};
    state.active = "code";
    bool switched = false;
    for (int i = 0; i < 8; ++i) {
        result = transition(state, implicit);
        state = result.next;
        if (result.effective == "code_review") { switched = true; break; }
    }
    assert(switched);
    // A first practical task may activate an ordinary role immediately even
    // when phrased as a goal, not a command starting with "Please" / "Make".
    State fresh;
    Decision first_plan{{"plan", .68}, {"default", .32}, Intent::Implicit, Scope::NewTask};
    result = transition(fresh, first_plan);
    assert(result.effective == "plan" && result.next.active == "plan");

    // Do not interpret a near tie as a clear goal, and never loosen far-away
    // transitions just because an unrelated first-turn rule exists.
    Decision uncertain{{"plan", .58}, {"default", .42}, Intent::Implicit, Scope::NewTask};
    result = transition(fresh, uncertain);
    assert(result.effective == "default" && result.next.active == "default");
    State reviewing;
    reviewing.active = "code_review";
    Decision vague_story{{"creative_write", .68}, {"default", .32}, Intent::Implicit, Scope::Continuation};
    result = transition(reviewing, vague_story);
    assert(result.effective == "code_review" && result.next.active == "code_review");


    for (const auto& role : kRoles)
        assert(!classifier_description(role.id).empty());

    // Both problem cases from the reported conversation, and a follow-up.
    auto pr = task_signal("I want to make a PR for a Lemonade feature", false);
    auto pr_menu = classifier_menu(pr, "default");
    assert(pr.direct_role.empty());
    assert(offered(pr_menu, "plan") && offered(pr_menu, "default"));
    assert(!offered(pr_menu, "write")); // PR is not a PR DESCRIPTION
    auto hut = task_signal("I want help with building a hut in my garden", false);
    auto hut_menu = classifier_menu(hut, "default");
    assert(offered(hut_menu, "plan") && !offered(hut_menu, "write"));
    auto mid_question = task_signal("I am working on a PR, what should I do?", false);
    assert(offered(classifier_menu(mid_question, "default"), "plan"));
    assert(!offered(classifier_menu(mid_question, "default"), "write"));
    auto clear_code = task_signal("It is a GUI and UX improvement and you have to provide me with the code", true);
    assert(clear_code.direct_role == "code");
    assert(clear_code.refers_to_previous);
    State active; active.active = "write";
    result = transition(active, direct_decision(clear_code));
    assert(result.effective == "code" && result.next.active == "code");
    active.active = "plan";
    result = transition(active, direct_decision(clear_code));
    assert(result.effective == "code" && result.next.active == "code");

    assert_direct("Please write the code and tests for this feature", "code");
    assert_direct("Design an executable plan for building a hut", "plan");
    assert_direct("You need to provide me with code for the feature", "code");
    assert_direct("Can you provide code for it?", "code", true);
    assert_direct("Kannst du einen Patch dazu schreiben?", "code", true);
    assert_direct("Du musst mir einen Patch liefern", "code");
    assert_direct("Write a PR description for this patch", "write", true);
    assert_direct("Schreibe bitte eine PR-Beschreibung", "write");
    assert_direct("Please write a report explaining how to implement this feature", "write");
    assert_direct("Write a Python script that checks the CSV", "code");
    assert_direct("Write a song about the moon", "creative_write");
    assert_direct("Review this PR for regressions", "code_review");
    assert_direct("Review the implementation for regressions", "code_review");
    assert_direct("Now write the PR description", "write", true);
    assert_direct("Formuliere eine E-Mail mit einer Terminanfrage", "write");
    assert_direct("Write a fantasy story about elves", "creative_write");
    assert(!task_signal("Write a fantasy story about elves", true).temporary);
    assert_direct("Translate this into German", "transform", true);
    assert_direct("Can you translate my document?", "transform", true);
    assert_direct("Please summarize the code review findings", "transform", true);
    assert_direct("Could you review the PR description for grammar?", "transform", true);
    assert_direct("Help me debug the failure", "diagnose", true);
    assert_direct("Erkläre mir den Algorithmus", "explain", true);
    assert_direct("Berechne die Lösung", "solve", true);
    assert_direct("Brainstorm different approaches", "explore");
    assert_direct("Liefere einen Patch zur Behebung des Fehlers", "code", true);
    assert_direct("Prüfe den PR auf Bugs", "code_review", true);

    // A transient writing task must not corrupt a long coding conversation.
    TaskSignal writing = task_signal("Now write the PR description", true);
    assert(writing.temporary);
    active.active = "code";
    result = transition(active, direct_decision(writing));
    assert(result.effective == "write" && result.next.active == "code" && result.temporary);

    // Quoted instructions and metalinguistic questions must not force routing.
    assert(task_signal("What does \"write code\" mean?", false).direct_role.empty());
    assert(task_signal("What does \"provide me with the code\" mean?", false).direct_role.empty());
    assert(task_signal("How do I write code for a solver?", false).direct_role.empty());
    assert(task_signal("Write a screenplay script about a detective", false).direct_role.empty());
    assert(task_signal("Please write our code of conduct", false).direct_role.empty());
    assert(task_signal("I want to learn to write code", false).direct_role.empty());
    assert(task_signal("Please explain how to write the code", false).direct_role == "explain");
    assert(task_signal("Can you explain how to implement this?", false).direct_role.empty() ||
           task_signal("Can you explain how to implement this?", false).direct_role == "explain");
    assert(task_signal("My product has a feature called Code Review", false).direct_role.empty());
    assert(task_signal("The ticket says implement the feature and deliver it", true).direct_role.empty());
    assert(task_signal("He told me to write the code yesterday", true).direct_role.empty());
    assert(task_signal("The logs say you have to provide me with the code", true).direct_role.empty());
    auto quoted = task_signal("What does \"write code\" mean?", false);
    assert(offered(classifier_menu(quoted, "default"), "explain"));

    // Without a reliable action cue, all 14 methods remain available.
    auto unfamiliar = task_signal("Hello! Could you assist with zorbic floom?", false);
    assert(classifier_menu(unfamiliar, "default").size() == kRoles.size());
    auto follow = task_signal("Can you do that now?", true);
    assert(follow.refers_to_previous);
    assert(classifier_input("Can you do that now?", "Build a shed", follow).find("Build a shed") != std::string::npos);
    auto independent = task_signal("Research sodium batteries", true);
    assert(!independent.refers_to_previous);
    assert(classifier_input("Research sodium batteries", "Build a shed", independent).find("Build a shed") == std::string::npos);

    // Confidence is computed across ALL offered options. The prior top-two
    // renormalisation would incorrectly turn 0.40 / 0.20 into 0.67 / 0.33.
    const std::vector<size_t> indices = {0, role_index("plan"), role_index("write"), role_index("code")};
    auto d = score_decision({0.10, 0.40, 0.20, 0.30}, indices, task_signal("a goal", false));
    assert(d.valid && d.first.id == "plan" && d.second.id == "code");
    assert(d.first.score > 0.39 && d.first.score < 0.41);
    assert(d.second.score > 0.29 && d.second.score < 0.31);
    assert(!score_decision({0, 0, 0, 0}, indices, {}).valid);
    assert(!score_decision({0.2, 0.4, 0.3, 1.5}, indices, {}).valid);
    assert(!score_decision({0.2, 0.4, 0.3, 0.1}, {0, 1, 1, 6}, {}).valid);
    assert(!score_decision({0.1, 0.9}, {role_index("code"), role_index("write")}, {}).valid);
    // A realistic five-way zero-shot outcome may have no >0.6 score.
    // Independent lexical evidence + the model's clear lead can still start
    // planning without fabricating a top-two 0.8 confidence value.
    auto supported = task_signal("I want to make a PR for a Lemonade feature", false);
    auto menu = classifier_menu(supported, "default");
    std::vector<double> values(menu.size(), 0.05);
    for (size_t i = 0; i < menu.size(); ++i) {
        if (kRoles[menu[i]].id == "plan") values[i] = 0.43;
        if (kRoles[menu[i]].id == "default") values[i] = 0.25;
    }
    d = score_decision(values, menu, supported);
    assert(d.valid && d.first.id == "plan" && d.task_supported);
    result = transition(State{}, d);
    assert(result.effective == "plan");
    // Almost tied scores do NOT become a decisive classification just because
    // the lexical retriever also happened to propose planning.
    for (size_t i = 0; i < menu.size(); ++i) {
        if (kRoles[menu[i]].id == "plan") values[i] = 0.31;
        if (kRoles[menu[i]].id == "default") values[i] = 0.30;
    }
    d = score_decision(values, menu, supported);
    result = transition(State{}, d);
    assert(result.effective == "default");

    // New, strong independent task switches; weak continuation stays put.
    active.active = "write";
    d = score_decision({0.05, 0.04, 0.01, 0.90}, indices, task_signal("new unrelated request", true));
    assert(d.valid && d.scope == Scope::NewTask);
    result = transition(active, d);
    assert(result.effective == "code" && result.next.active == "code");
    d.scope = Scope::Continuation;
    d.first.score = 0.55;
    d.second.score = 0.45;
    result = transition(active, d);
    assert(result.effective == "write" && result.next.active == "write");


    std::cout << "smart role controller tests passed\n";
}
