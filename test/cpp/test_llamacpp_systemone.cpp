#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::backends::llamacpp::build_systemone_classify_request;
using lemon::backends::llamacpp::build_systemone_decisions_request;
using lemon::backends::llamacpp::laya_option_texts;
using lemon::backends::llamacpp::systemone_decisions_response;
using lemon::backends::llamacpp::decision_model_load_refusal;
using lemon::backends::llamacpp::decision_text_truncated;
using lemon::backends::llamacpp::encoder_decision_ctx_size;
using lemon::backends::llamacpp::laya_label_overflow;
using lemon::backends::llamacpp::map_encoder_overflow_error;
using lemon::backends::llamacpp::plan_single_batch;
using lemon::backends::llamacpp::SingleBatchPlan;
using lemon::backends::llamacpp::systemone_classify_response;
using nlohmann::json;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "ok: " << message << std::endl;
        return;
    }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
}

template <typename Exception, typename Fn>
bool throws(Fn fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

json answer_with(json probabilities) {
    return json{{"answers", {{"label", {{"type", "choice"}, {"probabilities", probabilities}}}}}};
}

}  // namespace

int main() {
    const json labels = json::array({"coding", "pii", "cooking"});

    const nlohmann::ordered_json body = build_systemone_classify_request("bake a cheesecake", labels);
    expect(body["state"] == "bake a cheesecake", "the input text is the state");
    expect(body["questions"].size() == 1, "the body asks exactly one question");
    const nlohmann::ordered_json& question = body["questions"]["label"];
    expect(question["type"] == "choice", "the question is a choice");
    expect(question["instructions"].is_string() && !question["instructions"].empty(),
           "the question has instructions");
    expect(question["criteria"].dump() == R"({"coding":null,"pii":null,"cooking":null})",
           "each label is an option without a description, in the order of the labels");

    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array());
           }),
           "an empty label list is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", 7}));
           }),
           "a non-string label is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", "  "}));
           }),
           "a blank label is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", "a"}));
           }),
           "a duplicate label is refused");

    const json response =
        answer_with({{"coding", 0.0002}, {"pii", 0.0096}, {"cooking", 0.9888}});
    const json all = systemone_classify_response(response, labels, 0, false);
    expect(all["labels"].size() == 3, "every label is returned without top_k");
    expect(all["labels"]["cooking"] == 0.9888, "the probabilities are returned unchanged");
    expect(all.contains("truncated") && all["truncated"] == false,
           "the answer reports a text that was not cut");
    expect(!systemone_classify_response(response, labels, 0, std::nullopt).contains("truncated"),
           "the answer leaves truncated out when the cut is unknown");

    const json top = systemone_classify_response(response, labels, 1, false);
    expect(top["labels"].size() == 1 && top["labels"].contains("cooking"),
           "top_k keeps the highest scores");

    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(answer_with({{"coding", 0.5}, {"pii", 0.5}}), labels,
                                           0, false);
           }),
           "an answer without one of the labels is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 0.2}, {"pii", 0.2}, {"other", 0.6}}), labels, 0,
                   false);
           }),
           "an answer with a label that was not asked is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 2.0}, {"pii", 0.0}, {"cooking", 0.0}}), labels, 0,
                   false);
           }),
           "a probability outside [0, 1] is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(json{{"answers", json::object()}}, labels, 0, false);
           }),
           "a response without the answer is refused");

    const std::string long_text(40000, 'a');
    expect(decision_text_truncated("laya", long_text) == false,
           "laya refuses a long text instead of cutting it");
    expect(decision_text_truncated("lev", "short") == false, "lev does not cut the text");
    expect(decision_text_truncated("h2o-lightning", "short") == false,
           "h2o-lightning keeps a text under its state limit");
    expect(!decision_text_truncated("h2o-lightning", long_text).has_value(),
           "h2o-lightning may cut a long text");
    expect(!decision_text_truncated("lfm2-d1-omni", "short").has_value(),
           "lfm2-d1-omni may cut any text");
    expect(!decision_text_truncated("", "short").has_value(),
           "an unread decision type is not trusted");
    expect(!decision_text_truncated("acme-router", "short").has_value(),
           "a decision type lemond does not know is not trusted");

    const std::vector<std::string> three = {"coding", "pii", "cooking"};
    expect(laya_label_overflow(three, {2, 2, 2}, 192).empty(), "short labels fit");
    expect(laya_label_overflow({"a", "b"}, {48, 2}, 192).empty(),
           "a 48-token label is read whole");
    const std::string per_label = laya_label_overflow({"a", "b"}, {49, 2}, 192);
    expect(per_label.find("49 tokens") != std::string::npos &&
               per_label.find("48 tokens") != std::string::npos,
           "a 49-token label is refused, naming both numbers");
    const std::string head = laya_label_overflow({"a", "b", "c", "d"}, {45, 45, 45, 45}, 192);
    expect(head.find("184 tokens") != std::string::npos && head.find("176") != std::string::npos,
           "labels that overflow max_head_tokens are refused, naming both numbers");
    expect(laya_label_overflow({"a", "b", "c", "d"}, {43, 43, 43, 43}, 192).empty(),
           "labels that fill the head exactly are read whole");
    expect(laya_label_overflow(std::vector<std::string>(45, "x"), std::vector<size_t>(45, 3), 192)
               .empty(),
           "an over-full head whose options are already at the 4-token floor is not cut");
    expect(laya_label_overflow(three, {2, 2, 2}, 0).empty(),
           "an unknown max_head_tokens skips the check");

    expect(encoder_decision_ctx_size(190000, 8192) == 8192,
           "a context above the trained window is capped to it");
    expect(encoder_decision_ctx_size(2048, 8192) == 2048, "a smaller context is kept");
    expect(encoder_decision_ctx_size(0, 8192) == 8192, "an unset context takes the trained window");
    expect(encoder_decision_ctx_size(4096, 0) == 4096, "an unknown trained window changes nothing");

    const auto plan = [](const std::vector<std::string>& user_args) {
        return plan_single_batch(8192, user_args);
    };
    const auto plan_is = [](const SingleBatchPlan& p, bool add_b, bool add_ub, int limit) {
        return p.add_batch_size == add_b && p.add_ubatch_size == add_ub &&
               p.max_input_tokens == limit;
    };
    expect(plan_is(plan({}), true, true, 8192),
           "without batch flags, -b and -ub are added at the context size");
    expect(plan_is(plan({"-b", "8192"}), false, true, 8192),
           "-b alone still gets -ub at the context size");
    expect(plan_is(plan({"-ub", "8192"}), true, false, 8192),
           "-ub alone still gets -b at the context size");
    expect(plan_is(plan({"-b", "2048"}), false, true, 2048), "a smaller -b limits the input");
    expect(plan_is(plan({"-ub", "1024"}), true, false, 1024), "a smaller -ub limits the input");
    expect(plan_is(plan({"-b", "2048", "-ub", "1024"}), false, false, 1024),
           "with both flags set, nothing is added and the smaller one is the limit");
    expect(plan_is(plan({"--flash-attn", "on", "--batch-size=4096"}), false, true, 4096),
           "the long form with '=' counts as set");
    expect(plan_is(plan({"--ubatch-size", "16384"}), true, false, 8192),
           "a flag above the context does not raise the limit");
    expect(plan_is(plan({"-b", "abc"}), false, true, 8192),
           "an unreadable value still counts as set");

    const SingleBatchPlan wide = plan_single_batch(262144, {});
    expect(wide.batch_size == 8192 && wide.max_input_tokens == 8192,
           "a large context alone keeps the batch at 8192");
    const SingleBatchPlan raised = plan_single_batch(262144, {"-b", "32768", "-ub", "32768"});
    expect(raised.max_input_tokens == 32768, "-b and -ub raise the batch");
    const SingleBatchPlan one = plan_single_batch(262144, {"-ub", "32768"});
    expect(one.add_batch_size && one.batch_size == 32768 && one.max_input_tokens == 32768,
           "raising one flag sizes the one lemond adds");
    expect(plan_single_batch(4096, {}).batch_size == 4096,
           "a context below 8192 keeps the batch at the context");

    expect(decision_model_load_refusal("Laya", false, "modern-bert", "laya").empty(),
           "a decision GGUF loads");
    expect(decision_model_load_refusal("D3", false, "qwen35", "vllm-d3").empty(),
           "a vllm-d3 GGUF loads");
    const std::string fork = decision_model_load_refusal("Kai", false, "qwen3", "decision2");
    expect(fork.find("'decision2'") != std::string::npos &&
               fork.find("does not support") != std::string::npos,
           "a decision type the pinned llama.cpp does not know is refused, naming it");
    expect(decision_model_load_refusal("LayaHF", true, "", "").find("hf_load") !=
               std::string::npos,
           "hf_load is refused, because lemond cannot read the GGUF header");
    expect(decision_model_load_refusal("Chat", false, "qwen3", "").find("no decision head") !=
               std::string::npos,
           "a GGUF without a decision head is refused");
    expect(decision_model_load_refusal("Unread", false, "", "").empty(),
           "an unread header is not refused here");

    const json overflow = {{"error", {
        {"message", "input (9217 tokens) is too large to process. increase the physical batch "
                    "size (current batch size: 8192)"},
        {"type", "server_error"},
        {"status_code", 500},
    }}};
    const json mapped = map_encoder_overflow_error(overflow, 8192);
    expect(mapped["error"]["status_code"] == 400 &&
               mapped["error"]["code"] == "context_length_exceeded",
           "an input longer than the batch is a 400 context_length_exceeded");
    expect(mapped["error"]["message"].get<std::string>().find("input (9217 tokens)") == 0 &&
               mapped["error"]["message"].get<std::string>().find("8192 tokens") !=
                   std::string::npos,
           "the message names the input and the window sizes");
    const json other = {{"error", {{"message", "Compute error."}, {"status_code", 500}}}};
    expect(map_encoder_overflow_error(other, 8192) == other, "other errors are left alone");
    expect(map_encoder_overflow_error(response, 8192) == response, "answers are left alone");

    {
        using ordered = nlohmann::ordered_json;
        const ordered decisions = ordered::parse(R"({
            "model": "Laya-GGUF",
            "input": "I was charged twice.",
            "questions": [
                {"type": "predicate", "name": "angry", "instructions": "Is the customer angry?"},
                {"type": "choice", "name": "team", "instructions": "Which team?",
                 "choices": [{"value": "shipping"}, {"value": "billing", "description": "payments"},
                             {"value": false}]},
                {"type": "score", "instructions": [{"type": "input_text", "text": "How urgent?"}],
                 "levels": [{"label": "low"}, {"label": "high", "description": "today"}]}
            ]
        })");
        const ordered body = build_systemone_decisions_request(decisions);
        expect(body["state"] == "I was charged twice.", "a string input is the state");
        expect(!body.contains("images"), "no images key without images");
        expect(body["questions"].dump() ==
                   R"({"q0":{"type":"noul","instructions":"Is the customer angry?"},)"
                   R"("q1":{"type":"choice","instructions":"Which team?","criteria":)"
                   R"({"shipping":null,"billing":"payments","false":null}},)"
                   R"("q2":{"type":"score","instructions":"How urgent?","criteria":)"
                   R"(["low","high: today"]}})",
               "predicate, choice and score map to noul, choice and score, in order");

        const auto texts = laya_option_texts(body);
        expect(texts.size() == 2 &&
                   texts[0] == std::vector<std::string>{"shipping", "billing: payments", "false"} &&
                   texts[1] == std::vector<std::string>{"level 0: low", "level 1: high: today"},
               "laya option texts follow the laya template");

        ordered messages = decisions;
        messages["input"] = ordered::parse(R"([
            {"role": "user", "content": "first"},
            {"type": "message", "role": "user", "content": [
                {"type": "input_text", "text": "second"},
                {"type": "input_image", "image_url": "data:image/png;base64,AAAA"},
                {"type": "input_text", "text": "third"}]}
        ])");
        const ordered message_body = build_systemone_decisions_request(messages);
        expect(message_body["state"] == "first\n\nsecond\nthird",
               "parts join with a new line and messages with a blank line");
        expect(message_body["images"] == ordered::array({"data:image/png;base64,AAAA"}),
               "a data-URL image goes to images");

        ordered json_input = decisions;
        json_input["input"] = ordered::parse(
            R"([{"role": "user", "content": [{"type": "input_json", "data": {"b": 1, "a": 2}}]}])");
        expect(build_systemone_decisions_request(json_input)["state"].dump() == R"({"b":1,"a":2})",
               "one input_json part stays JSON, in its key order");

        ordered image_only = decisions;
        image_only["input"] = ordered::parse(
            R"([{"role": "user", "content": [{"type": "input_image", "image_url": "data:x"}]}])");
        expect(build_systemone_decisions_request(image_only)["state"].is_null(),
               "an image-only input has a null state");

        auto refused = [&](const char* path, const ordered& value, const std::string& message) {
            ordered bad = decisions;
            bad[nlohmann::ordered_json::json_pointer(path)] = value;
            expect(throws<std::invalid_argument>([&] { build_systemone_decisions_request(bad); }),
                   message);
        };
        refused("/input", ordered::parse(R"([{"role": "developer", "content": "x"}])"),
                "a developer message is refused");
        refused("/input", ordered::parse(
                    R"([{"role": "user", "content": [{"type": "input_image", "image_url": "https://x/y.png"}]}])"),
                "an image URL that is not a data URL is refused");
        refused("/questions", ordered::array(), "an empty question list is refused");
        refused("/questions/0/type", "maybe", "an unknown question type is refused");
        refused("/questions/0/criteria", ordered::object(), "a /v1/systemone field is refused");
        refused("/questions/2/name", "angry", "a repeated question name is refused");
        refused("/questions/1/choices", ordered::parse(R"([{"value": "only"}])"),
                "a choice with one option is refused");
        refused("/questions/1/choices/0/value", "false",
                "a string and a boolean with the same text are refused");
        refused("/questions/2/levels", ordered::parse(R"([{"label": "one"}])"),
                "a score with one level is refused");
        refused("/questions/0/instructions",
                ordered::parse(R"([{"type": "input_image", "image_url": "data:x"}])"),
                "an image in question instructions is refused");

        const ordered systemone_answer = ordered::parse(R"({
            "model": "Laya-GGUF",
            "answers": {
                "q0": {"type": "noul", "noul": 0.8},
                "q1": {"type": "choice", "choice": "false", "confidence": 0.5,
                       "probabilities": {"shipping": 0.1, "billing": 0.2, "false": 0.7}},
                "q2": {"type": "score", "score": 0.75, "confidence": 0.4,
                       "legend": {"0": "low", "1": "high: today"},
                       "probabilities": {"0": 0.25, "1": 0.75}}
            },
            "usage": {"input_tokens": 90, "output_tokens": 0}
        })");
        const ordered result = systemone_decisions_response(systemone_answer, decisions, "Laya-GGUF");
        expect(result["answers"][0].dump() ==
                   R"({"type":"predicate","name":"angry","probability":0.8})",
               "a noul answers the predicate");
        expect(result["answers"][1]["choice"] == false &&
                   result["answers"][1]["probabilities"].dump() ==
                       R"([{"value":"shipping","probability":0.1},{"value":"billing","probability":0.2},)"
                       R"({"value":false,"probability":0.7}])",
               "choice values keep their type and the request order");
        expect(result["answers"][2]["name"].is_null() && result["answers"][2]["score"] == 0.75 &&
                   result["answers"][2]["probabilities"][1].dump() ==
                       R"({"value":1,"label":"high","probability":0.75})",
               "a score answer lists each level by index and label");
        expect(result["usage"].dump() ==
                   R"({"input_tokens":90,"input_tokens_details":{"cached_tokens":0,"cache_write_tokens":0},)"
                   R"("output_tokens":0,"output_tokens_details":{"reasoning_tokens":0},"total_tokens":90})",
               "usage has OpenAI's shape");
        ordered missing = systemone_answer;
        missing["answers"].erase("q2");
        expect(throws<std::runtime_error>(
                   [&] { systemone_decisions_response(missing, decisions, "Laya-GGUF"); }),
               "a missing answer is refused");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "all checks passed" << std::endl;
    return EXIT_SUCCESS;
}
