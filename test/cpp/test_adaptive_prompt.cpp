#include "lemon/server/adaptive_prompt.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::adaptive_prompt::json;
using lemon::adaptive_prompt::Mode;

namespace {

int failures = 0;
void check(const char* name, bool result) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
    if (!result) ++failures;
}

json request(const std::string& input) {
    return {{"model", "test-model"}, {"messages", json::array({
        {{"role", "user"}, {"content", input}},
    })}};
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool rejects_catalog(const json& doc) {
    try { (void)lemon::adaptive_prompt::parse_catalog(doc); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

bool rejects_option(const json& option) {
    try { (void)lemon::adaptive_prompt::parse_options(option); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

} // namespace

int main() {
    using namespace lemon::adaptive_prompt;
    const auto modules = builtin_catalog();
    check("builtin library is populated", modules.size() >= 200);
    check("builtins include roles and skills", std::any_of(modules.begin(), modules.end(),
          [](const Module& m) { return m.id == "coder" && m.type == "role"; }) &&
          std::any_of(modules.begin(), modules.end(),
          [](const Module& m) { return m.id == "tribology" && m.type == "skill"; }));

    check("false is off", parse_options(false).mode == Mode::Off);
    check("true is rules", parse_options(true).mode == Mode::Rules);
    check("llm option parsed", parse_options(json{{"mode", "llm"}}).mode == Mode::Llm);
    check("manual options parsed", parse_options(json{{"mode", "manual"},
                                                  {"ids", json::array({"coder"})},
                                                  {"trace", true}}).trace);
    check("separate decision model parsed", parse_options(json{{"mode", "llm"},
               {"decision_model", "Qwen3-0.6B-GGUF"}}).decision_model == "Qwen3-0.6B-GGUF");
    const auto carrying = parse_options(json{{"mode", "rules"},
        {"previous_ids", json::array({"creative-writing", "story_characterization"})}});
    check("conversation state parsed", carrying.previous_ids.size() == 2);
    check("invalid previous IDs rejected", rejects_option(json{{"mode", "rules"},
        {"previous_ids", json::array({"invalid ID"})}}));
    check("manual selection cannot inherit previous IDs", rejects_option(json{{"mode", "manual"},
        {"ids", json::array({"reviewer"})}, {"previous_ids", json::array({"coder"})}}));
    check("decision model only allowed for LLM mode", rejects_option(json{{"mode", "rules"},
               {"decision_model", "Qwen3-0.6B-GGUF"}}));
    check("bad selector mode rejected", rejects_option(json{{"mode", "super"}}));
    check("bad manual IDs rejected", rejects_option(json{{"mode", "manual"}}));
    check("wrong ID field rejected", rejects_option(json{{"mode", "rules"},
                                                         {"ids", json::array({"coder"})}}));
    check("unknown option key rejected", rejects_option(json{{"foo", "bar"}}));
    check("trace type checked", rejects_option(json{{"trace", "yes"}}));

    {
        const auto body = request("Please review this code for bugs.");
        const auto selected = select_ids(last_user_text(body), modules);
        check("review selects reviewer", contains(selected, "reviewer"));
        check("review does not select coder", !contains(selected, "coder"));
    }
    {
        const auto pr = select_ids("I need a PR review", modules);
        check("short PR review selects reviewer", contains(pr, "reviewer"));
        check("short PR review with article variant", contains(select_ids("I need an PR review", modules),
              "reviewer"));
        check("short PR review does not select coder", !contains(pr, "coder"));
        check("review my PR selects reviewer", contains(select_ids("Review my PR", modules), "reviewer"));
        check("math discussion gets topical skill", contains(select_ids("Talk to me about math", modules),
              "mathematics_discussion"));
        check("math conversation is not forced into a calculation role",
              !contains(select_ids("Talk to me about math", modules), "mathematical_solver"));
        check("health issues gets medical guidance", contains(select_ids("I have health issues", modules),
              "medical_information"));
    }
    {
        const auto selected = select_ids("Schreib eine Geschichte mit einem Roboter.", modules);
        check("German creative writing", contains(selected, "creative-writing"));
    }
    {
        const auto selected = select_ids("Berechne die Hertzsche Pressung und erkläre mir warum.", modules);
        check("numerical skill combined", contains(selected, "numerical-accuracy"));
        check("scientific skill combined", contains(selected, "scientific-reasoning"));
        check("only one role and two skills", selected.size() <= 3);
    }
    {
        check("unrelated conversation gets no forced prompt", select_ids("Hello there!", modules).empty());
        check("German uppercase umlaut matches", phrase_match(lowercase_utf8("PRÜFE DEN CODE"),
                 lowercase_utf8("prüfe den code")));
        check("keyword boundaries prevent partial collisions", !phrase_match("token-275", "token-27"));
        check("Han phrase matches in continuous Chinese", phrase_match("请帮我审查这段代码", "审查"));
        check("Chinese review selects reviewer", contains(select_ids("请审查这段代码", modules), "reviewer"));
        check("Chinese story selects creative-writing", contains(select_ids("请写一个故事", modules), "creative-writing"));
        check("Chinese characterization selects skill", contains(select_ids("重点是人物塑造", modules), "story_characterization"));
        check("Chinese math discussion selects skill", contains(select_ids("聊聊数学", modules), "mathematics_discussion"));
        check("Latin word matching unchanged near Chinese", !phrase_match("请查看token-275", "token-27"));
        check("Chinese unrelated request does not force review", !contains(select_ids("今天的天气如何", modules), "reviewer"));
    }
    {
        check("long unformatted code leaves trailing instruction visible",
              contains(select_ids(std::string(5000, 'x') + "\nPlease review this patch.", modules), "reviewer"));
        const auto first = select_ids("Schreib eine Geschichte", modules);
        const auto followup = merge_ids(first, select_ids("Mir ist Charakterentwicklung wichtig", modules), modules);
        check("colloquial story request selects writer",
              contains(select_ids("Ich will eine Story", modules), "creative-writing"));
        check("follow-up retains creative role", contains(followup, "creative-writing"));
        check("follow-up adds character skill", contains(followup, "story_characterization"));
        check("no duplicate role on repeated follow-up",
              merge_ids(followup, {"creative-writing"}, modules).size() == followup.size());
        check("empty follow-up leaves stable prompt selection", merge_ids(followup, {}, modules) == followup);
        const auto new_task = merge_ids(followup, {"reviewer"}, modules);
        check("new role clears old story instructions", new_task.size() == 1 &&
              contains(new_task, "reviewer"));
        const auto detail = merge_ids({"detailed_explanation"}, {"concise_answer"}, modules);
        check("latest conflicting style wins", detail.size() == 1 &&
              contains(detail, "concise_answer"));
        const auto combined = merge_ids({"creative-writing", "story_dialogue"},
                                        {"story_characterization"}, modules);
        check("compatible role and two story skills coexist", combined.size() == 3);
        const auto full = merge_ids(combined, {"story_pacing"}, modules);
        check("latest focus replaces oldest skill when limit reached", full.size() == 3 &&
              contains(full, "story_pacing") && contains(full, "creative-writing"));

        auto body = request("Continue the story");
        const auto applied = apply_ids(body, modules, followup);
        apply_ids(body, modules, applied);
        const auto sys = body["messages"].front()["content"].get<std::string>();
        check("combined system prompt stays at beginning", body["messages"].front()["role"] == "system");
        check("same role inserted exactly once", sys.find("[creative-writing]") != std::string::npos &&
              sys.find("[creative-writing]") == sys.rfind("[creative-writing]"));
        check("same skills inserted exactly once", sys.find("[story_characterization]") != std::string::npos &&
              sys.find("[story_characterization]") == sys.rfind("[story_characterization]"));
    }
    {
        auto body = request("Please review this code.");
        const auto original = body;
        const auto result = apply_ids(body, modules, {});
        check("no selected modules changes no message", result.empty() &&
              body["messages"] == original["messages"]);
        check("extension stripped before forwarding", !body.contains("adaptive_prompt"));
    }
    {
        auto body = request("Review the code");
        body["adaptive_prompt"] = true;
        const auto first = apply_ids(body, modules, {"reviewer"});
        const auto twice = apply_ids(body, modules, {"reviewer"});
        check("system role used", first.size() == 1 && body["messages"][0]["role"] == "system");
        check("reapplying does not duplicate message", twice.size() == 1 && body["messages"].size() == 2);
        const auto content = body["messages"][0]["content"].get<std::string>();
        check("idempotent system guidance", content.find("[LEMONADE_ADAPTIVE_PROMPT_START]") ==
                  content.rfind("[LEMONADE_ADAPTIVE_PROMPT_START]"));
        check("user message preserved", body["messages"].back()["content"] == "Review the code");
        check("extension not forwarded", !body.contains("adaptive_prompt"));
        apply_ids(body, modules, {});
        check("clearing selection removes injected system message", body["messages"].size() == 1 &&
              body["messages"][0]["role"] == "user");
    }
    {
        auto a = request("Check this for bugs");
        auto b = a;
        const auto left = apply_ids(a, modules, {"factual-precision", "reviewer"});
        const auto right = apply_ids(b, modules, {"reviewer", "factual-precision"});
        check("instruction order canonicalized for cache stability", left == right &&
              a["messages"] == b["messages"] && left.front() == "reviewer");
    }
    {
        auto body = request("Review this code");
        body["messages"].insert(body["messages"].begin(),
                               json{{"role", "system"}, {"content", "Client instruction"}});
        apply_ids(body, modules, {"reviewer"});
        check("client system text preserved", body["messages"].size() == 2 &&
              body["messages"][0]["content"].get<std::string>().find("Client instruction") == 0);
        apply_ids(body, modules, {"coder"});
        const auto content = body["messages"][0]["content"].get<std::string>();
        check("guidance replaced rather than stacked", content.find("[reviewer]") == std::string::npos &&
              content.find("[coder]") != std::string::npos);
        apply_ids(body, modules, {});
        check("original system restored", body["messages"][0]["content"] == "Client instruction");
    }
    {
        auto body = request("Review this code");
        body["messages"].front()["content"] = json::array({
            {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAA"}}}},
            {{"type", "text"}, {"text", "Review this Python code"}},
        });
        check("multimodal user text extracted", last_user_text(body).find("Review this Python") !=
                                                  std::string::npos);
        apply_ids(body, modules, {"reviewer"});
        check("multimodal message preserved", body["messages"].back()["content"].is_array());
    }
    {
        auto body = request("And now fix it.");
        body["messages"].insert(body["messages"].begin(),
            json{{"role", "user"}, {"content", "Review this code carefully."}});
        check("prior user turn extracted for follow-up", previous_user_text(body) ==
              "Review this code carefully.");
        const auto chosen = decide_with_model(last_user_text(body), modules, "test-model",
            [](const json& decision_request) -> json {
                const auto text = decision_request["messages"][1]["content"].get<std::string>();
                if (text.find("Review this code") == std::string::npos ||
                    text.find("And now fix it") == std::string::npos) {
                    throw std::runtime_error("missing follow-up context");
                }
                return {{"choices", json::array({
                    {{"message", {{"content", "{\"ids\":[\"coder\"]}"}}}}
                })}};
            }, previous_user_text(body));
        check("follow-up decision can change role", contains(chosen, "coder"));
    }
    {
        const std::string input = "```python\n# write code and implement stuff\n```\nPlease review this patch.";
        const auto selected = select_ids(input, modules);
        check("code-fence content not treated as user intent", contains(selected, "reviewer") &&
              !contains(selected, "coder"));
        check("words after fence retained", intent_excerpt(input).find("review this patch") !=
                                               std::string::npos);
    }
    {
        auto parsed = parse_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"review","type":"role","keywords":["review"],"prompt":"Review carefully.","excludes":["rewrite"]},
            {"id":"rewrite","type":"skill","keywords":["review"],"prompt":"Rewrite everything."},
            {"id":"compact","type":"skill","keywords":["review"],"prompt":"Be concise.","group":"style"},
            {"id":"verbose","type":"skill","keywords":["review"],"prompt":"Be lengthy.","group":"style"}
        ]})"));
        const auto selected = select_ids("review", parsed);
        check("both directions of exclusion enforced", contains(selected, "review") &&
              !contains(selected, "rewrite"));
        check("compatible skill selected", contains(selected, "compact"));
        check("skill group is exclusive", !contains(selected, "verbose"));
        const auto forced = constrain_ids({"rewrite", "review"}, parsed);
        check("manual conflicts cannot bypass exclusions", forced.size() == 1);
    }
    {
        auto body = request("Please review this code.");
        const auto selected = decide_with_model(last_user_text(body), modules, "test-model",
            [](const json& decision_request) -> json {
                if (decision_request.value("stream", true) ||
                    decision_request.value("max_tokens", 0) > 128) {
                    throw std::runtime_error("unexpected decision request");
                }
                return {{"choices", json::array({
                    {{"message", {{"content", "{\"ids\":[\"reviewer\",\"factual-precision\"]}"}}}}
                })}};
            });
        check("decision only outputs catalog IDs", contains(selected, "reviewer") &&
              contains(selected, "factual-precision"));
        bool rejected = false;
        try {
            (void)decide_with_model("review code", modules, "test-model", [](const json&) -> json {
                return {{"choices", json::array({
                    {{"message", {{"content", "{\"ids\":[\"invented\"]}"}}}}
                })}};
            });
        } catch (const std::runtime_error&) { rejected = true; }
        check("decision cannot invent module IDs", rejected);
    }
    {
        const auto classifier_modules = parse_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"reviewer","type":"role","keywords":["review"],"description":"Review code","prompt":"Review the code carefully."},
            {"id":"factual-precision","type":"skill","keywords":["review"],"description":"Check factual correctness","prompt":"Avoid unsupported claims."}
        ]})"));
        int calls = 0;
        const auto selected = decide_with_classifier(
            "Please review this patch for bugs", classifier_modules, "d3-lite-FLM",
            [&calls](const json& req) -> json {
                ++calls;
                if (req.value("model", std::string()) != "d3-lite-FLM" ||
                    req.value("input", std::string()).find("review") == std::string::npos) {
                    throw std::runtime_error("bad classifier input");
                }
                json scores = json::object();
                for (const auto& label : req["labels"]) {
                    const std::string name = label.get<std::string>();
                    scores[name] = name.find("reviewer:") == 0 ? 0.89 :
                        name.find("factual-precision:") == 0 ? 0.73 :
                        name.find("None of these") == 0 ? 0.04 : 0.01;
                }
                return {{"labels", scores}};
            });
        check("classifier queries two independent label sets", calls == 2);
        check("classifier picks trusted reviewer role", contains(selected, "reviewer"));
        check("classifier combines a compatible skill", contains(selected, "factual-precision"));
    }
    {
        // A 16-skill zero-shot menu can give its winning label under 10% even
        // when the classifier clearly prefers it to the no-match alternative.
        // Use a normalized probability distribution, as d3-lite-FLM does.
        const auto selected = decide_with_classifier(
            "I would like an approachable introduction to mathematical intuition",
            modules, "d3-lite-FLM", [](const json& req) -> json {
                json scores = json::object();
                bool math_skill_menu = false;
                for (const auto& value : req["labels"]) {
                    const std::string label = value.get<std::string>();
                    if (label.find("mathematics_discussion:") == 0) math_skill_menu = true;
                }
                const double other = math_skill_menu
                    ? 0.855 / static_cast<double>(req["labels"].size() - 2)
                    : 0.25 / static_cast<double>(req["labels"].size() - 1);
                for (const auto& value : req["labels"]) {
                    const std::string label = value.get<std::string>();
                    scores[label] = math_skill_menu
                        ? (label.find("mathematics_discussion:") == 0 ? 0.09 :
                           label.find("None of these") == 0 ? 0.055 : other)
                        : (label.find("None of these") == 0 ? 0.75 : other);
                }
                return { {"labels", scores} };
            });
        check("classifier accepts best skill under ten percent", contains(selected,
              "mathematics_discussion"));
    }
    {
        // A classifier may be overconfident in 'none' for a short, explicit
        // task. Preserve the trusted keyword match as a conservative rescue.
        const auto classify_none = [](const json& req) -> json {
            json scores = json::object();
            for (const auto& value : req["labels"]) {
                const std::string label = value.get<std::string>();
                scores[label] = label.find("None of these") == 0 ? 0.75 :
                                0.25 / static_cast<double>(req["labels"].size() - 1);
            }
            return { {"labels", scores} };
        };
        check("classifier abstention still selects explicit PR reviewer",
            contains(decide_with_classifier("I need a PR review", modules, "d3-lite-FLM", classify_none),
                     "reviewer"));
        check("classifier abstention still selects math discussion",
            contains(decide_with_classifier("Talk to me about math", modules, "d3-lite-FLM", classify_none),
                     "mathematics_discussion"));
        check("classifier abstention still selects health information",
            contains(decide_with_classifier("I have health issues", modules, "d3-lite-FLM", classify_none),
                     "medical_information"));
    }
    {
        int calls = 0;
        const auto selected = decide_with_classifier("Unrelated greeting", modules, "decision",
            [&calls](const json& req) -> json {
                ++calls;
                json scores = json::object();
                for (const auto& label : req["labels"]) {
                    const std::string name = label.get<std::string>();
                    scores[name] = name.find("None of these") == 0 ? 0.90 : 0.005;
                }
                return {{"labels", scores}};
            });
        check("no lexical candidates still calls decision classifier", calls == 2);
        check("classifier can explicitly select nothing", selected.empty());
        check("no-match remains valid for unrelated messages", selected.empty());
        bool rejected = false;
        try {
            (void)decide_with_classifier("review code", modules, "decision",
                [](const json&) { return json{{"labels", json::object()}}; });
        } catch (const std::runtime_error&) { rejected = true; }
        check("classifier missing labels refuses rather than guessing", rejected);
    }
    {
        json doc = {{"version", 1}, {"modules", json::array()}};
        for (int i = 0; i < 300; ++i) {
            doc["modules"].push_back({{"id", "custom-" + std::to_string(i)}, {"type", "skill"},
                {"description", "Task-specific instructions for token-" + std::to_string(i)},
                {"keywords", json::array({"token-" + std::to_string(i)})},
                {"prompt", "Trusted instruction"}});
        }
        const auto catalog300 = parse_catalog(doc);
        const auto selected = select_ids("Use token-275", catalog300);
        check("hundreds of modules supported", catalog300.size() == 300 &&
              contains(selected, "custom-275"));
        const auto chosen = decide_with_model("Use token-275", catalog300, "test-model",
            [](const json& decision_request) -> json {
                const auto& msg = decision_request["messages"][1]["content"].get_ref<const std::string&>();
                if (msg.find("custom-275") == std::string::npos ||
                    msg.find("custom-200") != std::string::npos) {
                    throw std::runtime_error("bad candidate shortlist");
                }
                return {{"choices", json::array({
                    {{"message", {{"content", "{\"ids\":[\"custom-275\"]}"}}}}
                })}};
            });
        check("LLM shortlist finds relevant module among 300", contains(chosen, "custom-275"));
    }
    {
        check("catalog rejects duplicate IDs", rejects_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"dup","type":"role","keywords":["test"],"prompt":"A"},
            {"id":"dup","type":"skill","keywords":["test"],"prompt":"B"}]})")));
        check("catalog rejects unknown exclusions", rejects_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"first","type":"skill","keywords":["test"],"prompt":"A","excludes":["missing"]}]})")));
        check("catalog rejects oversized priority", rejects_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"first","type":"skill","keywords":["test"],"prompt":"A","priority":1000}]})")));
        check("catalog rejects unknown module key", rejects_catalog(json::parse(R"({"version":1,"modules":[
            {"id":"first","type":"skill","keywords":["test"],"promtp":"typo","prompt":"A"}]})")));
        check("catalog rejects unsupported version", rejects_catalog(json{{"version", 2}, {"modules", json::array()}}));
    }
    return failures ? 1 : 0;
}
