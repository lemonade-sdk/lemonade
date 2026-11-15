#include "lemon/server/hybrid_worker_policy.h"

#include <iostream>
#include <string>
#include <vector>

using namespace lemon::hybrid_worker_policy;

namespace {
int failed = 0;
void expect(bool ok, const char* name) {
    std::cout << (ok ? "PASS" : "FAIL") << ": " << name << '\n';
    failed += !ok;
}
template <typename Fn> bool rejects(Fn&& fn) {
    try { fn(); } catch (const std::runtime_error&) { return true; }
    return false;
}
}

int main() {
    const std::string done = "hybrid_worker_done";
    const std::set<std::string> names = {"bash", "edit", "read", "write"};
    expect(classify({"edit"}, done) == Outcome::External,
           "a Pi edit call is external");
    expect(classify({"bash", "read"}, done) == Outcome::External,
           "parallel external calls preserve their tool group");
    expect(classify({done}, done) == Outcome::Done,
           "an internal worker completion stays internal");
    expect(classify_reply({}, done, "Changed src/example.cpp; tests passed.", "stop") == Outcome::TextDone,
           "normal final text is a private worker handoff");
    expect(classify_reply({done}, done, "", "tool_calls") == Outcome::Done,
           "explicit internal worker_done remains valid");
    expect(classify_reply({"edit"}, done, "ignored", "tool_calls") == Outcome::External,
           "external tool calls still win over accompanying text");
    expect(rejects([&] { classify_reply({}, done, "", "stop"); }),
           "empty Worker text does not count as completion");
    expect(rejects([&] { classify_reply({}, done, "  \n \t ", "stop"); }),
           "whitespace-only Worker text does not count as completion");
    expect(rejects([&] { classify_reply({}, done, "Completed partially", "length"); }),
           "length-limited Worker text is not silently accepted");
    expect(rejects([&] { classify_reply({}, done, "some text", "content_filter"); }),
           "abnormal finishes cannot masquerade as Worker completion");
    expect(rejects([&] { classify_reply({"read", done}, done, "", "tool_calls"); }),
           "mixed private and external tools remain forbidden on automatic tool choice");
    expect(rejects([&] { classify({}, done); }),
           "worker cannot finish without choosing a tool");
    expect(rejects([&] { classify({""}, done); }),
           "an unnamed tool is rejected");
    expect(rejects([&] { classify({"edit", done}, done); }),
           "an internal completion cannot be mixed after an external edit");
    expect(rejects([&] { classify({done, "bash"}, done); }),
           "an internal completion cannot be mixed before an external command");
    expect(rejects([&] { classify({done, done}, done); }),
           "duplicate internal completions are rejected");
    validate_external({"bash", "read"}, {"id1", "id2"}, names);
    expect(rejects([&] { validate_external({"bash", "edit"}, {"id1", "id1"}, names); }),
           "duplicate IDs cannot reach the client");
    expect(rejects([&] { validate_external({"bash"}, {""}, names); }),
           "missing tool IDs cannot reach the client");
    expect(rejects([&] { validate_external({"bash", "read"}, {"id1"}, names); }),
           "partial tool lists cannot reach the client");
    expect(rejects([&] { validate_external({"unsafe"}, {"id1"}, names); }),
           "undeclared worker tool calls cannot reach the client");
    expect(possible_name_prefix("hybrid_", names, done),
           "partial internal names are buffered while streaming");
    expect(possible_name_prefix("edi", names, done),
           "partial external names are buffered while streaming");
    expect(!possible_name_prefix("hybrid_delegate_task", names, done),
           "orchestrator internals cannot appear as a Worker tool");
    expect(!possible_name_prefix("delete_all", names, done),
           "undeclared external tools are rejected");
    expect(unusable_visible_reply(false, false),
           "a thinking-only context-length reply is not valid completion");
    expect(!unusable_visible_reply(true, false),
           "ordinary text is still a valid partial length response");
    expect(!unusable_visible_reply(false, true),
           "proper tool call with no text is valid");
    return failed ? 1 : 0;
}
