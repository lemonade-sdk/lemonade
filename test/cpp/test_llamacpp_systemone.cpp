#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::backends::llamacpp::build_systemone_classify_request;
using lemon::backends::llamacpp::decision_model_load_refusal;
using lemon::backends::llamacpp::decision_text_truncated;
using lemon::backends::llamacpp::encoder_decision_ctx_size;
using lemon::backends::llamacpp::laya_cut;
using lemon::backends::llamacpp::laya_question_texts;
using lemon::backends::llamacpp::LayaQuestionText;
using lemon::backends::llamacpp::map_encoder_overflow_error;
using lemon::backends::llamacpp::mark_answers_truncated;
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

LayaQuestionText options_of(std::vector<std::string> options) {
    return LayaQuestionText{"q", "choice question: ?", std::move(options)};
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
    json reported = response;
    reported["answers"]["label"]["truncated"] = false;
    const json all = systemone_classify_response(reported, labels, 0);
    expect(all["labels"].size() == 3, "every label is returned without top_k");
    expect(all["labels"]["cooking"] == 0.9888, "the probabilities are returned unchanged");
    expect(all.contains("truncated") && all["truncated"] == false,
           "the answer's truncated reaches the classify response");
    expect(!systemone_classify_response(response, labels, 0).contains("truncated"),
           "an answer without truncated leaves it out");

    const json top = systemone_classify_response(response, labels, 1);
    expect(top["labels"].size() == 1 && top["labels"].contains("cooking"),
           "top_k keeps the highest scores");

    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(answer_with({{"coding", 0.5}, {"pii", 0.5}}), labels,
                                           0);
           }),
           "an answer without one of the labels is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 0.2}, {"pii", 0.2}, {"other", 0.6}}), labels, 0);
           }),
           "an answer with a label that was not asked is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 2.0}, {"pii", 0.0}, {"cooking", 0.0}}), labels, 0);
           }),
           "a probability outside [0, 1] is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(json{{"answers", json::object()}}, labels, 0);
           }),
           "a response without the answer is refused");

    const nlohmann::ordered_json answers = nlohmann::ordered_json::parse(
        R"({"answers": {"a": {"type": "noul", "noul": 0.9}, "b": {"type": "noul", "noul": 0.1}}})");
    nlohmann::ordered_json marked = answers;
    mark_answers_truncated(marked, "laya", nlohmann::ordered_json{{"state", "short"}});
    expect(marked["answers"]["a"]["truncated"] == false &&
               marked["answers"]["b"]["truncated"] == false,
           "each answer of a model that refuses long text says it was not cut");
    marked = answers;
    mark_answers_truncated(marked, "lfm2-d1-omni", nlohmann::ordered_json{{"state", "short"}});
    expect(marked == answers, "an answer that may have been cut gets no truncated key");
    marked = answers;
    mark_answers_truncated(marked, "h2o-lightning",
                           nlohmann::ordered_json{{"state", std::string(40000, 'a')}});
    expect(marked == answers, "a long h2o-lightning state gets no truncated key");
    nlohmann::ordered_json refused = {{"error", {{"message", "no"}}}};
    mark_answers_truncated(refused, "laya", nlohmann::ordered_json{{"state", "short"}});
    expect(!refused.contains("answers"), "an error response is left alone");

    const auto texts = laya_question_texts(nlohmann::ordered_json::parse(R"({
        "state": "x",
        "questions": {
            "team": {"type": "choice", "instructions": "Which team?",
                     "criteria": {"shipping": "where an order is", "billing": null}},
            "list": {"type": "choice", "instructions": "Which?",
                     "criteria": ["a", {"b": "the second"}]},
            "urgency": {"type": "score", "instructions": "How urgent?", "criteria": ["low", "high"]},
            "angry": {"type": "noul", "instructions": {"ask": "angry?"}},
            "refund": {"type": "noul", "instructions": "Refund?", "criteria": {"true": "asks for money back"}}
        }})"));
    expect(texts.size() == 5, "every question gets its laya text");
    expect(texts[0].question == "choice question: Which team?" &&
               texts[0].options == std::vector<std::string>{"shipping: where an order is", "billing"},
           "a choice option is 'key: description' or the key, in request order");
    expect(texts[1].options == std::vector<std::string>{"a", "b: the second"},
           "an array choice keeps its order");
    expect(texts[2].options == std::vector<std::string>{"level 0: low", "level 1: high"},
           "a score level is 'level i: text'");
    expect(texts[3].question == R"(noul question: {"ask":"angry?"})" &&
               texts[3].options == std::vector<std::string>{"true: yes, the statement holds",
                                                            "false: no, the statement does not hold"},
           "a noul without descriptions gets the template's yes and no texts");
    expect(texts[4].options[0] == "true: asks for money back",
           "a noul description replaces the template text");

    const auto three = options_of({"coding", "pii", "cooking"});
    expect(laya_cut(three, {2, 2, 2}, 4, 192).empty(), "short options fit");
    expect(laya_cut(options_of({"a", "b"}), {48, 2}, 4, 192).empty(),
           "a 48-token option is read whole");
    const std::string per_option = laya_cut(options_of({"a", "b"}), {49, 2}, 4, 192);
    expect(per_option.find("49 tokens") != std::string::npos &&
               per_option.find("48 tokens") != std::string::npos,
           "a 49-token option is refused, naming both numbers");
    const auto four = options_of({"a", "b", "c", "d"});
    const std::string head = laya_cut(four, {45, 45, 45, 45}, 4, 192);
    expect(head.find("184 tokens") != std::string::npos && head.find("176") != std::string::npos,
           "options that overflow max_head_tokens are refused, naming both numbers");
    expect(laya_cut(four, {43, 43, 43, 43}, 16, 192).empty(),
           "options that fill the head exactly are read whole");
    expect(laya_cut(options_of(std::vector<std::string>(45, "x")), std::vector<size_t>(45, 3), 8,
                    192).empty(),
           "an over-full head whose options are already at the 4-token floor is not cut");
    expect(laya_cut(options_of({"a", "b"}), {2, 2}, 186, 192).empty(),
           "a question that fits the room the options leave is read whole");
    const std::string question_cut = laya_cut(options_of({"a", "b"}), {2, 2}, 187, 192);
    expect(question_cut.find("187 tokens") != std::string::npos &&
               question_cut.find("186") != std::string::npos,
           "a question longer than the room the options leave is refused, naming both numbers");
    expect(laya_cut(three, {2, 2, 2}, 4, 0).empty(), "an unknown max_head_tokens skips the check");

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
    expect(plan_is(plan({}), true, true, 4096),
           "without batch flags, -b and -ub are added at 4096");
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
    expect(plan_is(plan({"-b", "abc"}), false, true, 4096),
           "an unreadable value still counts as set");

    const SingleBatchPlan wide = plan_single_batch(262144, {});
    expect(wide.batch_size == 4096 && wide.max_input_tokens == 4096,
           "a large context alone keeps the batch at 4096");
    const SingleBatchPlan raised = plan_single_batch(262144, {"-b", "32768", "-ub", "32768"});
    expect(raised.max_input_tokens == 32768, "-b and -ub raise the batch");
    const SingleBatchPlan one = plan_single_batch(262144, {"-ub", "32768"});
    expect(one.add_batch_size && one.batch_size == 32768 && one.max_input_tokens == 32768,
           "raising one flag sizes the one lemond adds");
    expect(plan_single_batch(2048, {}).batch_size == 2048,
           "a context below 4096 keeps the batch at the context");

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

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "all checks passed" << std::endl;
    return EXIT_SUCCESS;
}
