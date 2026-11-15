#include "lemon/server/hybrid_history.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using nlohmann::json;
using lemon::hybrid_history::canonical_history_message;
using lemon::hybrid_history::history_messages_equivalent;

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}

json call(const std::string& id, const std::string& name, const json& arguments) {
    return {{"id", id}, {"type", "function"},
            {"function", {{"name", name}, {"arguments", arguments}}}};
}

json assistant(const json& calls, const std::string& content = "") {
    return {{"role", "assistant"}, {"content", content}, {"tool_calls", calls}};
}

bool same(const json& expected, const json& received) {
    return history_messages_equivalent(canonical_history_message(expected),
                                       canonical_history_message(received));
}

template <typename Fn>
bool rejects(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    const json original = assistant(json::array({call("tool-1", "edit",
        R"({"path":"demo.py","edits":[{"oldText":"xyz", "newText":"abc"}]})")}));
    const json whitespace = assistant(json::array({call("tool-1", "edit",
        R"( { "path" : "demo.py", "edits" : [{ "oldText" : "xyz", "newText" : "abc" }] } )")}));
    check("JSON whitespace is equivalent", same(original, whitespace));

    const json reorder_keys = assistant(json::array({call("tool-1", "edit",
        R"({"edits":[{"newText":"abc","oldText":"xyz"}],"path":"demo.py"})")}));
    check("JSON object key order is equivalent", same(original, reorder_keys));

    const json changed_value = assistant(json::array({call("tool-1", "edit",
        R"({"path":"demo.py","edits":[{"oldText":"xyz","newText":"evil"}]})")}));
    check("Actual argument change is refused", !same(original, changed_value));

    const json changed_id = assistant(json::array({call("other-tool", "edit",
        R"({"path":"demo.py","edits":[{"oldText":"xyz","newText":"abc"}]})")}));
    check("Tool-call ID change is refused", !same(original, changed_id));
    const json changed_name = assistant(json::array({call("tool-1", "bash",
        R"({"path":"demo.py","edits":[{"oldText":"xyz","newText":"abc"}]})")}));
    check("Tool function name change is refused", !same(original, changed_name));

    check("Malformed JSON is rejected even when identical", rejects([&] {
        same(assistant(json::array({call("tool-1", "edit", "{not json")})),
             assistant(json::array({call("tool-1", "edit", "{not json")})));
    }));
    check("Non-string argument payload is rejected", rejects([&] {
        same(original, assistant(json::array({call("tool-1", "edit", json::object())})));
    }));
    check("JSON scalar cannot masquerade as a malformed argument", rejects([&] {
        same(assistant(json::array({call("tool-1", "edit", "hello")})),
             assistant(json::array({call("tool-1", "edit", R"("hello")")})));
    }));

    // A typical Pi edit stream has spaces after some colons; Pi echoes the
    // parsed arguments as compact JSON on its next OpenAI-compatible request.
    const std::string pi_stream = R"({"edits":[{"newText": "        qs = model._base_manager.using(using).filter(\n", "oldText": "        qs = model._default_manager.using(using).filter(\n"}], "path": "/repo/django/db/models/fields/related.py"})";
    const std::string pi_echo = json::parse(pi_stream).dump();
    check("Pi edit serialization differs in bytes", pi_stream != pi_echo);
    check("Pi edit tool call is unchanged semantically",
          same(assistant(json::array({call("pi-edit-42", "edit", pi_stream)})),
               assistant(json::array({call("pi-edit-42", "edit", pi_echo)}))));

    const json tool_result = {{"role", "tool"}, {"tool_call_id", "pi-edit-42"},
                              {"content", "Successfully replaced 1 block(s)"}};
    const json tool_result_parts = {{"role", "tool"}, {"tool_call_id", "pi-edit-42"},
                                    {"content", json::array({{{"type", "text"},
                                                               {"text", "Successfully replaced 1 block(s)"}}})}};
    check("Pi edit result text parts are equivalent", same(tool_result, tool_result_parts));
    const json modified_result = {{"role", "tool"}, {"tool_call_id", "pi-edit-42"},
                                  {"content", "Changed result"}};
    check("Changed tool result is refused", !same(tool_result, modified_result));
    const json modified_result_id = {{"role", "tool"}, {"tool_call_id", "other"},
                                     {"content", "Successfully replaced 1 block(s)"}};
    check("Tool result ID change is refused", !same(tool_result, modified_result_id));

    const json changed_user = {{"role", "user"}, {"content", "modified instructions"}};
    check("Rewritten user instruction is refused",
          !same({{"role", "user"}, {"content", "original instructions"}}, changed_user));
    check("Role change is refused", !same(tool_result, {{"role", "user"},
                                                            {"content", "Successfully replaced 1 block(s)"}}));
    const json a = call("a", "read", R"({"path":"a.py"})");
    const json b = call("b", "bash", R"({"command":"echo test"})");
    check("Reordered parallel tool calls are refused",
          !same(assistant(json::array({a, b})), assistant(json::array({b, a}))));
    check("Missing parallel tool call is refused",
          !same(assistant(json::array({a, b})), assistant(json::array({a}))));
    check("Duplicate tool call is refused",
          !same(assistant(json::array({a, b})), assistant(json::array({a, a}))));

    json altered_type = assistant(json::array({a}));
    altered_type["tool_calls"][0]["type"] = "other";
    check("Changed tool type is refused", !same(assistant(json::array({a})), altered_type));

    // Copilot/Kilo on the same loaded llama.cpp model must be able to start a
    // distinct first-turn conversation without inheriting another client's KV.
    const json stored_chat = json::array({
        {{"role", "system"}, {"content", "Kilo instructions"}},
        {{"role", "user"}, {"content", "Implement helper A"}},
        {{"role", "assistant"}, {"content", "Helper A is implemented"}},
    });
    const json new_copilot_chat = json::array({
        {{"role", "system"}, {"content", "Copilot instructions"}},
        {{"role", "user"}, {"content", "Implement helper B"}},
    });
    check("Distinct fresh Copilot conversation requests a full slot reset",
          lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, new_copilot_chat));
    check("System content changes still fail the strict message comparator",
          !same(stored_chat[0], new_copilot_chat[0]));
    check("Different first user request starts a new session even with same system",
          lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, json::array({
                  {{"role", "system"}, {"content", "Kilo instructions"}},
                  {{"role", "user"}, {"content", "Different first turn"}},
              })));
    check("Identical initial prefix is NOT silently treated as fresh",
          !lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, json::array({stored_chat[0], stored_chat[1]})));
    check("Unchanged continuation is not mistaken for a new chat",
          !lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, json::array({stored_chat[0], stored_chat[1],
                                       stored_chat[2],
                                       json{{"role", "user"}, {"content", "Follow up"}}})));
    check("Changed system prompt in ongoing chat still fails strict history check",
          !lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, json::array({new_copilot_chat[0], stored_chat[1],
                                       stored_chat[2]})));
    check("Old conversation tool results never trigger a new session",
          !lemon::hybrid_history::is_distinct_fresh_conversation(
              stored_chat, json::array({new_copilot_chat[0], new_copilot_chat[1],
                                       json{{"role", "tool"}, {"tool_call_id", "x"},
                                            {"content", "OK"}}})));
    check("Mid-dialogue system injection is not a fresh chat",
          !lemon::hybrid_history::is_fresh_conversation_request(json::array({
              stored_chat[1], new_copilot_chat[0], new_copilot_chat[1]})));
    check("Fresh start allows multiple ordered leading system/developer messages",
          lemon::hybrid_history::is_fresh_conversation_request(json::array({
              json{{"role", "system"}, {"content", "Instructions 1"}},
              json{{"role", "developer"}, {"content", "Instructions 2"}},
              json{{"role", "user"}, {"content", "Question"}}})));
    check("No tools or assistant messages allowed in fresh-start detection",
          !lemon::hybrid_history::is_fresh_conversation_request(json::array({
              stored_chat[0], stored_chat[1], stored_chat[2]})));

    const json prior = json::array({
        json{{"role", "system"}, {"content", "Rules"}},
        json{{"role", "user"}, {"content", "Original task"}},
        assistant(json::array({call("first", "read", R"({"path":"x.py"})")})),
        json{{"role", "tool"}, {"tool_call_id", "first"}, {"content", "x"}},
        assistant(json::array({call("second", "edit", R"({"path":"y.py"})")})),
    });
    json pruned = prior;
    pruned[3]["content"] = "[Old tool result content cleared]";
    check("Known Kilo pruning may replace completed tool output",
          lemon::hybrid_history::same_or_pruned(prior[3], pruned[3], true));
    check("Kilo pruning must not change a pending tool ID",
          !lemon::hybrid_history::same_or_pruned(prior[3],
              json{{"role", "tool"}, {"tool_call_id", "forged"},
                   {"content", "[Old tool result content cleared]"}}, true));
    const json compacted = json::array({
        prior[0], json{{"role", "user"}, {"content", "Earlier work summary"}},
        prior[3], prior[4],
        json{{"role", "tool"}, {"tool_call_id", "second"}, {"content", "done"}},
    });
    check("Exact retained suffix permits a stable-ID compaction",
          lemon::hybrid_history::retained_suffix_end(prior, compacted) == 4);
    json corrupted = compacted;
    corrupted[3]["tool_calls"][0]["id"] = "forged";
    check("Compacted client cannot forge outstanding call ID",
          !lemon::hybrid_history::retained_suffix_end(prior, corrupted));
    corrupted = compacted;
    corrupted[0]["content"] = "Different developer rules";
    check("Compaction cannot replace original system instructions",
          !lemon::hybrid_history::retained_suffix_end(prior, corrupted));

    return failures == 0 ? 0 : 1;
}
