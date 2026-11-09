#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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

// The token window ort-server gives the ONNX export of the same family (its
// manifest.json max_length), so both backends cut the same text the same way.
constexpr int RULE_HEAD_ROUTER_CTX_SIZE = 2048;

// llama-server reads ctx_size 0 as the GGUF's window, the base model's 128000,
// and lemond sizes the batch from ctx_size, so 0 would give a batch of 0.
int rule_head_ctx_size(int requested);

// A local origin, `file://`, or a rooted path. Decided by shape, not by whether
// the file exists: a missing file is still a path, never the repository "C" or
// "/models/r.gguf", and a relative "org/repo" is a repository even when a
// folder of that name sits in lemond's working directory.
bool checkpoint_is_local(const std::string& checkpoint, bool local_origin);

// A GGUF and its rule head reach lemond the same way, both from disk or both
// from a registry. Empty when they do, else why not.
std::string mixed_rule_head_source_error(const std::string& model_name,
                                         const std::string& main_checkpoint,
                                         const std::string& rule_head, bool local_origin);

// A local file's head is the file beside it, and a local directory's head is in
// it; a remote checkpoint is `[scheme://]repo:file`, so its head is RULE_HEAD_FILE
// in the same repository.
std::string implied_rule_head_checkpoint(const std::string& main_checkpoint, bool local_origin);

std::string missing_rule_head_message(const std::string& model_name, const std::string& checkpoint);

// Ranges are in bytes, because llama-server's token pieces are bytes.
struct RouterInput {
    std::string text;
    std::vector<std::pair<size_t, size_t>> label_ranges;
    size_t text_start = 0;
    // The elision marker lemond wrote into a cut text. Its tokens are the
    // server's text, not the user's, so the text pool leaves them out.
    size_t marker_start = 0;
    size_t marker_end = 0;
};

RouterInput build_router_input(const std::vector<std::string>& labels, const std::string& text);

// Tokenizes without special tokens and returns llama-server's token array with pieces.
using PieceTokenizer = std::function<nlohmann::json(const std::string&)>;

struct RouterFit {
    RouterInput input;
    nlohmann::json tokens;  // input.text from the tokenizer, kept for router_tokens()
    bool truncated = false;
};

RouterFit fit_router_input(const std::vector<std::string>& labels, const std::string& text,
                           size_t window, size_t n_specials, size_t elision_tokens,
                           const PieceTokenizer& tokenize);

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
// `with_special` may hold bare ids; `without_special` must carry pieces.
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

nlohmann::json router_classify_response(const std::vector<std::string>& labels,
                                        const std::vector<double>& probabilities, int top_k,
                                        bool truncated);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
