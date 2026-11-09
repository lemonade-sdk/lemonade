#include "lemon/backends/llamacpp/llamacpp_rulehead.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using lemon::backends::llamacpp::build_router_input;
using lemon::backends::llamacpp::fit_router_input;
using lemon::backends::llamacpp::group_router_tokens;
using lemon::backends::llamacpp::checkpoint_is_local;
using lemon::backends::llamacpp::implied_rule_head_checkpoint;
using lemon::backends::llamacpp::local_directory_checkpoint_error;
using lemon::backends::llamacpp::mixed_rule_head_source_error;
using lemon::backends::llamacpp::rule_head_ctx_size;
using lemon::backends::llamacpp::parse_rule_head;
using lemon::backends::llamacpp::router_classify_response;
using lemon::backends::llamacpp::router_tokens;
using lemon::backends::llamacpp::RouterFit;
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

// One token per ASCII character; a multi-byte character splits into two tokens
// that are not whole UTF-8, as a byte-level BPE splits an emoji.
json char_tokens(const std::string& s) {
    json out = json::array();
    for (size_t i = 0; i < s.size();) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        const size_t n = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        if (n == 1) {
            out.push_back(piece(lead, std::string(1, s[i])));
        } else {
            json rest = json::array();
            for (size_t k = 1; k < n; ++k) rest.push_back(static_cast<unsigned char>(s[i + k]));
            out.push_back(piece(1000, json::array({lead})));
            out.push_back(piece(1001, rest));
        }
        i += n;
    }
    return out;
}

std::string repeat(const std::string& s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
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
    {
        json bare = json::array({1});
        for (const auto& token : without) bare.push_back(token["id"]);
        const RouterTokens from_ids = router_tokens(bare, without, input.text.size());
        expect(from_ids.ids == tokens.ids && from_ids.spans == tokens.spans,
               "ids without pieces place BOS the same way");
    }

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
    {
        // An untagged path is local when it exists, as ArtifactSourceResolver decides.
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("lemon_test_llamacpp_rulehead_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "router-f16.gguf") << "GGUF";
        const std::string base = dir.generic_u8string();
        expect(implied_rule_head_checkpoint(base + "/router-f16.gguf", false) ==
                   base + "/rulehead.bin",
               "an untagged local path's head is the file beside the GGUF");

        // Without a Windows drive, so the path has no colon on any OS, as on Linux.
        const std::string posix = (dir.root_directory() / dir.relative_path()).generic_u8string();
        expect(local_directory_checkpoint_error("user.r", base).find("names the folder") !=
                       std::string::npos &&
                   !local_directory_checkpoint_error("user.r", posix).empty() &&
                   !local_directory_checkpoint_error("user.r", base + "/").empty() &&
                   !local_directory_checkpoint_error("user.r", "file://" + base).empty(),
               "a folder checkpoint is refused in words, as llama-server loads one file");
        expect(local_directory_checkpoint_error("user.r", base + "/router-f16.gguf").empty() &&
                   local_directory_checkpoint_error("user.r", base + "/not-here.gguf").empty() &&
                   local_directory_checkpoint_error("user.r", "").empty(),
               "a GGUF file, a missing path, or no path is not a folder");
        expect(implied_rule_head_checkpoint(base + "/", true) == base + "/rulehead.bin",
               "a trailing separator gets no second one");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    // Local by shape, not by existence: a missing file is still a path.
    expect(implied_rule_head_checkpoint("/models/not-here/router-f16.gguf", false) ==
               "/models/not-here/rulehead.bin",
           "a rooted path that does not exist yet keeps its head beside it");
#ifdef _WIN32
    expect(implied_rule_head_checkpoint(R"(D:\not-here\router-f16.gguf)", false) ==
               R"(D:\not-here\rulehead.bin)",
           "a missing Windows path never becomes the repository \"D\"");
#endif
    {
        const std::filesystem::path cwd = std::filesystem::current_path();
        const std::filesystem::path scratch =
            std::filesystem::temp_directory_path() /
            ("lemon_test_rulehead_cwd_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(scratch / "org" / "router-GGUF");
        std::filesystem::current_path(scratch);
        expect(!checkpoint_is_local("org/router-GGUF", false) &&
                   implied_rule_head_checkpoint("org/router-GGUF:router-f16.gguf", false) ==
                       "org/router-GGUF:rulehead.bin",
               "a repository id stays remote when a folder of that name is in the working directory");
        std::filesystem::current_path(cwd);
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }

    expect(mixed_rule_head_source_error("user.r", "/models/r-f16.gguf", "/models/rulehead.bin",
                                        false)
                   .empty() &&
               mixed_rule_head_source_error("user.r", "org/r-GGUF:r-f16.gguf",
                                            "other/heads:rulehead.bin", false)
                   .empty(),
           "a GGUF and head from the same kind of place are accepted");
    {
        const std::string local_gguf = mixed_rule_head_source_error(
            "user.r", "/models/r-f16.gguf", "org/r-GGUF:rulehead.bin", false);
        const std::string remote_gguf = mixed_rule_head_source_error(
            "user.r", "org/r-GGUF:r-f16.gguf", "/models/rulehead.bin", false);
        expect(local_gguf.find("local GGUF but a remote") != std::string::npos &&
                   remote_gguf.find("remote GGUF but a local") != std::string::npos,
               "a local GGUF with a remote head, or the reverse, is refused in words");
    }
    expect(mixed_rule_head_source_error("user.r", "user.r/r-f16.gguf", "org/r-GGUF:rulehead.bin",
                                        true)
               .empty(),
           "under a local source every checkpoint is a path, so nothing is mixed");

    expect(rule_head_ctx_size(0) == 2048 && rule_head_ctx_size(-1) == 2048,
           "ctx_size 0 means the router's own window, not the GGUF's 128000");
    expect(rule_head_ctx_size(512) == 512 && rule_head_ctx_size(8192) == 8192,
           "an explicit ctx_size is kept");
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

    const json top = router_classify_response(labels, probabilities, 2, false);
    expect(top["labels"].size() == 2 && top["labels"].contains("code") &&
               top["labels"].contains("math") && top["truncated"] == false,
           "top_k keeps the highest labels and reports no cut");
    expect(router_classify_response(labels, probabilities, 0, true)["truncated"] == true,
           "a cut is reported");

    // ort-server 0.4.0's budget, with one token per ASCII character: the label
    // block is 23 tokens, BOS is 1, and the elision marker is 5.
    {
        const std::string marker = "\n...\n";
        const std::string e_acute = "\xC3\xA9";
        const std::vector<std::string> one = {"a"};
        const std::string digits = repeat("0123456789", 10);

        const RouterFit fits = fit_router_input(one, "short text", 64, 1, 5, char_tokens);
        expect(!fits.truncated && fits.input.text == build_router_input(one, "short text").text,
               "a text that fits is read whole");

        // Budget 64 - 1 - 23 = 40; less the marker, 35: head 35 / 3 = 11, tail 24.
        const RouterFit cut = fit_router_input(one, digits, 64, 1, 5, char_tokens);
        const std::string kept = digits.substr(0, 11) + marker + digits.substr(100 - 24);
        expect(cut.truncated && cut.input.text == build_router_input(one, kept).text,
               "a long text keeps a third of its budget from the head, the rest from the tail");
        expect(cut.input.marker_start == cut.input.text_start + 11 &&
                   cut.input.marker_end == cut.input.text_start + 16,
               "the marker's place is recorded");
        const auto spans = token_byte_spans(cut.tokens, cut.input.text.size());
        const RouterTokenGroups pooled = group_router_tokens(cut.input, spans, one);
        expect(pooled.text.size() == 35, "the marker is left out of the text pool");

        // Budget 39, keep 34: head 11 and tail 23 tokens, both odd, so each would
        // end inside a two-token character; the head gives one back, the tail too.
        const RouterFit chars =
            fit_router_input(one, repeat(e_acute, 100), 63, 1, 5, char_tokens);
        expect(chars.input.text ==
                   build_router_input(one, repeat(e_acute, 5) + marker + repeat(e_acute, 11))
                       .text,
               "a cut lands between characters, never inside one");

        // Budget 6 cannot pay for a 5-token marker and keep both ends: head 2, tail 4.
        const RouterFit tight = fit_router_input(one, digits, 30, 1, 5, char_tokens);
        expect(tight.truncated &&
                   tight.input.text == build_router_input(one, "01" + digits.substr(96)).text &&
                   tight.input.marker_start == tight.input.marker_end,
               "a budget too small for the marker drops it");

        expect(throws<std::invalid_argument>(
                   [&] { fit_router_input(one, digits, 26, 1, 5, char_tokens); }),
               "a label block that leaves no room for text is refused, never cut");
        expect(throws<std::invalid_argument>([&] {
                   fit_router_input({repeat("x", 100)}, "short", 64, 1, 5, char_tokens);
               }),
               "a label longer than the window is refused");
    }

    if (failures > 0) {
        std::cerr << failures << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all rule head tests passed" << std::endl;
    return 0;
}
