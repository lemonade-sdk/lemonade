#include "lemon/backends/llamacpp/llamacpp_rulehead.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using lemon::backends::llamacpp::build_router_input;
using lemon::backends::llamacpp::group_router_tokens;
using lemon::backends::llamacpp::implied_rule_head_checkpoint;
using lemon::backends::llamacpp::parse_rule_head;
using lemon::backends::llamacpp::router_classify_response;
using lemon::backends::llamacpp::router_tokens;
using lemon::backends::llamacpp::RouterInput;
using lemon::backends::llamacpp::RouterTokenGroups;
using lemon::backends::llamacpp::RouterTokens;
using lemon::backends::llamacpp::RuleHead;
using lemon::backends::llamacpp::score_router_labels;
using lemon::backends::llamacpp::token_byte_spans;
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

json piece(int id, json value) { return json{{"id", id}, {"piece", std::move(value)}}; }

std::string bytes_of(const std::vector<float>& floats) {
    std::string out(floats.size() * sizeof(float), '\0');
    std::memcpy(out.data(), floats.data(), out.size());
    return out;
}

// hidden 2, proj 2: both projections are the identity, so a label scores the
// cosine between its own mean vector and the text's.
std::vector<float> identity_head(float score_bias, float logit_scale) {
    return {1, 0, 0, 1,  0, 0,  1, 0, 0, 1,  0, 0,  score_bias, logit_scale};
}

}  // namespace

int main() {
    const std::vector<std::string> labels = {"code", "math", "chat"};
    const RouterInput input = build_router_input(labels, "Fix my Python bug");
    expect(input.text == "Categories:\n- code\n- math\n- chat\n\nText:\nFix my Python bug",
           "the input matches Lfm2BidirForSequenceRouting._prefix");
    expect(input.label_ranges == std::vector<std::pair<size_t, size_t>>{{14, 18}, {21, 25}, {28, 32}},
           "label ranges match _category_ranges");
    expect(input.text_start == 40, "the text starts after the Text: header");

    // The router's tokenizer: each label is one token with its leading space.
    const json without = json::array({
        piece(544, "C"), piece(32830, "ategories"), piece(1334, ":\n"), piece(522, "-"),
        piece(5214, " code"), piece(708, "\n"), piece(522, "-"), piece(10343, " math"),
        piece(708, "\n"), piece(522, "-"), piece(22537, " chat"), piece(509, "\n\n"),
        piece(6570, "Text"), piece(1334, ":\n"), piece(56603, "Fix"), piece(1727, " my"),
        piece(22017, " Python"), piece(23785, " bug")});
    json with = json::array({piece(1, "<|startoftext|>")});
    for (const auto& token : without) with.push_back(token);

    const RouterTokens tokens = router_tokens(with, without, input.text.size());
    expect(tokens.ids.size() == 19 && tokens.ids[0] == 1 && tokens.ids[5] == 5214,
           "ids keep BOS first");
    expect(tokens.spans[0] == std::pair<size_t, size_t>{0, 0}, "BOS gets an empty span");
    expect(tokens.spans[5] == std::pair<size_t, size_t>{13, 18}, "the label token keeps its space");

    const RouterTokenGroups groups = group_router_tokens(input, tokens.spans, labels);
    expect(groups.labels == std::vector<std::vector<size_t>>{{5}, {8}, {11}},
           "each label pools its own token");
    expect(groups.text == std::vector<size_t>{15, 16, 17, 18}, "the text pools its four tokens");

    {
        // "café ☕": é is 2 bytes and ☕ is 3, split over two tokens that are not
        // whole UTF-8, so llama-server sends them as byte arrays.
        const std::vector<std::string> mixed = {"code", "café ☕"};
        const RouterInput cafe = build_router_input(mixed, "Fix my bug");
        expect(cafe.label_ranges[1] == std::pair<size_t, size_t>{21, 30},
               "label ranges count bytes, not characters");
        const json pieces = json::array({
            piece(544, "C"), piece(32830, "ategories"), piece(1334, ":\n"), piece(522, "-"),
            piece(5214, " code"), piece(708, "\n"), piece(522, "-"), piece(35499, " café"),
            piece(40961, json::array({32, 226, 152})), piece(753, json::array({149})),
            piece(509, "\n\n"), piece(6570, "Text"), piece(1334, ":\n"), piece(56603, "Fix"),
            piece(1727, " my"), piece(23785, " bug")});
        const auto spans = token_byte_spans(pieces, cafe.text.size());
        expect(spans[8] == std::pair<size_t, size_t>{26, 29} &&
                   spans[9] == std::pair<size_t, size_t>{29, 30},
               "byte-array pieces count their bytes");
        const RouterTokenGroups split = group_router_tokens(cafe, spans, mixed);
        expect(split.labels[1] == std::vector<size_t>{7, 8, 9},
               "both halves of a split character pool with their label");
        expect(split.text == std::vector<size_t>{13, 14, 15}, "the text after a split label");
    }

    expect(throws<std::runtime_error>([&] { token_byte_spans(without, input.text.size() + 1); }),
           "pieces that do not cover the input are refused");
    expect(throws<std::runtime_error>([&] {
               router_tokens(json::array({piece(1, "<|startoftext|>"), piece(9, "x")}), without,
                             input.text.size());
           }),
           "a tokenization that differs with special tokens is refused");
    expect(throws<std::invalid_argument>([&] {
               group_router_tokens(input, {{0, 0}, {0, 40}}, labels);
           }),
           "an input with no text token is refused");
    expect(throws<std::invalid_argument>([&] {
               std::vector<std::pair<size_t, size_t>> spans = tokens.spans;
               spans[5] = {13, 13};
               group_router_tokens(input, spans, labels);
           }),
           "a label with no token is refused");

    // Scoring. Text vector (1, 0); "code" (1, 0), "math" (0, 1), "chat" (-1, 0).
    const RuleHead head = parse_rule_head(bytes_of(identity_head(0.5f, 0.0f)), 2);
    expect(head.hidden == 2 && head.proj == 2, "the head size follows from the encoder width");
    json rows = json::array();
    for (size_t i = 0; i < tokens.ids.size(); ++i) rows.push_back(json::array({0.0, 0.0}));
    rows[5] = {3.0, 0.0};
    rows[8] = {0.0, 2.0};
    rows[11] = {-1.0, 0.0};
    for (size_t i : groups.text) rows[i] = {1.0, 0.0};
    const std::vector<double> probabilities =
        score_router_labels(head, rows, tokens.ids.size(), groups);
    // logits 1.5, 0.5, -0.5: the bias cancels in the softmax.
    const double z = std::exp(1.0) + 1.0 + std::exp(-1.0);
    expect(std::abs(probabilities[0] - std::exp(1.0) / z) < 1e-12 &&
               std::abs(probabilities[1] - 1.0 / z) < 1e-12 &&
               std::abs(probabilities[2] - std::exp(-1.0) / z) < 1e-12,
           "scores are softmax(cos * e^logit_scale + bias)");

    {
        // Each projection has a different non-symmetric matrix and a bias, so a
        // transposed or swapped matrix, or a dropped bias, changes every score.
        const RuleHead skew = parse_rule_head(
            bytes_of({1, 2, 0, 1,  0.5f, -1,  0, 1, 1, 0,  1, 0,  0, 0}), 2);
        const std::vector<double> got = score_router_labels(skew, rows, tokens.ids.size(), groups);
        // q = (1.5, -1); code (1, 3), math (3, 0), chat (1, -1)
        const double q = std::sqrt(3.25);
        const std::vector<double> logits = {-1.5 / (q * std::sqrt(10.0)), 4.5 / (q * 3.0),
                                            2.5 / (q * std::sqrt(2.0))};
        double sum = 0.0;
        for (double l : logits) sum += std::exp(l);
        bool close = true;
        for (size_t i = 0; i < logits.size(); ++i) {
            close = close && std::abs(got[i] - std::exp(logits[i]) / sum) < 1e-12;
        }
        expect(close, "an asymmetric head scores with tok_w and rule_w in place and their biases");
    }

    {
        std::vector<float> nan_weight = identity_head(0.0f, 0.0f);
        nan_weight[1] = std::nanf("");
        expect(throws<std::runtime_error>([&] { parse_rule_head(bytes_of(nan_weight), 2); }),
               "a head with a non-finite weight is refused");
    }

    expect(implied_rule_head_checkpoint("org/router-GGUF:router-f16.gguf", false) ==
               "org/router-GGUF:rulehead.bin",
           "a remote head is rulehead.bin in the same repository");
    expect(implied_rule_head_checkpoint("hf://org/router-GGUF:router-f16.gguf", false) ==
               "hf://org/router-GGUF:rulehead.bin",
           "a scheme-prefixed head keeps its scheme");
    expect(implied_rule_head_checkpoint("file:///models/router-f16.gguf", false) ==
               "file:///models/rulehead.bin",
           "a file:// head is the file beside the GGUF");
    expect(implied_rule_head_checkpoint("user.my-router/router-f16.gguf", true) ==
               "user.my-router/rulehead.bin",
           "a local upload's head is the file beside the GGUF");
    expect(implied_rule_head_checkpoint("C:\\models\\router-f16.gguf", true) ==
               "C:\\models\\rulehead.bin",
           "a Windows path keeps its drive and directory");

    const RuleHead capped = parse_rule_head(bytes_of(identity_head(0.0f, 10.0f)), 2);
    const std::vector<double> sharp = score_router_labels(capped, rows, tokens.ids.size(), groups);
    expect(std::abs(sharp[1] - 1.0 / (std::exp(30.0) + 1.0 + std::exp(-30.0))) < 1e-15,
           "the logit scale is capped at 30");

    expect(throws<std::runtime_error>([&] {
               parse_rule_head(bytes_of(identity_head(0.0f, 0.0f)), 3);
           }),
           "a head of the wrong width is refused");
    expect(throws<std::runtime_error>([&] { score_router_labels(head, rows, 18, groups); }),
           "a row count that differs from the tokens sent is refused");

    const json top = router_classify_response(labels, probabilities, 2);
    expect(top["labels"].size() == 2 && top["labels"].contains("code") &&
               top["labels"].contains("math") && top["truncated"] == false,
           "top_k keeps the highest labels and reports no cut");

    if (failures > 0) {
        std::cerr << failures << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all rule head tests passed" << std::endl;
    return 0;
}
