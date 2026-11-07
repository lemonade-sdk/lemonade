#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {

// An LFM2 prompt-router GGUF holds only the bidirectional encoder. Its
// head ships beside it as `rulehead.bin`, so llama-server returns one vector per
// token (--pooling none) and lemond pools the label and text spans and scores
// them, as Lfm2BidirForSequenceRouting.route() and ort-server's zero-shot task do.

constexpr const char* RULE_HEAD_FILE = "rulehead.bin";

std::string missing_rule_head_message(const std::string& model_name, const std::string& checkpoint);

// Ranges are in bytes, because llama-server's token pieces are bytes.
struct RouterInput {
    std::string text;
    std::vector<std::pair<size_t, size_t>> label_ranges;
    size_t text_start = 0;
};

RouterInput build_router_input(const std::vector<std::string>& labels, const std::string& text);

// A piece that is not whole UTF-8 (half an emoji) arrives as an array of byte
// values. Pieces that do not add up to the input are refused: drifted spans
// would pool the wrong tokens without any error.
std::vector<std::pair<size_t, size_t>> token_byte_spans(const nlohmann::json& tokens,
                                                        size_t text_bytes);

struct RouterTokens {
    std::vector<int64_t> ids;
    std::vector<std::pair<size_t, size_t>> spans;
};

// BOS's piece is "<|startoftext|>", text that is not in the input, so it is
// found by tokenizing with and without special tokens and given an empty span.
RouterTokens router_tokens(const nlohmann::json& with_special,
                           const nlohmann::json& without_special, size_t text_bytes);

struct RouterTokenGroups {
    std::vector<std::vector<size_t>> labels;
    std::vector<size_t> text;
};

RouterTokenGroups group_router_tokens(const RouterInput& input,
                                      const std::vector<std::pair<size_t, size_t>>& spans,
                                      const std::vector<std::string>& labels);

// rulehead.bin, float32 little-endian, no header:
// tok_w[proj][hidden] | tok_b[proj] | rule_w[proj][hidden] | rule_b[proj] | score_bias | logit_scale
struct RuleHead {
    int64_t hidden = 0;
    int64_t proj = 0;
    std::vector<float> tok_w;
    std::vector<float> tok_b;
    std::vector<float> rule_w;
    std::vector<float> rule_b;
    float score_bias = 0.0f;
    float logit_scale = 0.0f;
};

RuleHead parse_rule_head(const std::string& bytes, int64_t hidden);

std::vector<double> score_router_labels(const RuleHead& head, const nlohmann::json& rows,
                                        size_t n_tokens, const RouterTokenGroups& groups);

// `truncated` is always false: unlike ort-server, lemond never cuts the text,
// and llama-server refuses an input longer than its batch.
nlohmann::json router_classify_response(const std::vector<std::string>& labels,
                                        const std::vector<double>& probabilities, int top_k);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
