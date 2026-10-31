#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {

// /v1/classify with `labels` -> a /v1/systemone body that asks one `choice`
// question whose options are those labels, in their order. Throws
// std::invalid_argument unless `labels` is a non-empty array of distinct,
// non-blank strings.
nlohmann::ordered_json build_systemone_classify_request(const std::string& text,
                                                        const nlohmann::json& labels);

// What /v1/classify can say about a cut of `text` by a model of this decision
// type: false when llama-server keeps it whole or refuses it, empty when it may
// cut without saying so (lfm2-d1-omni, a long text on h2o-lightning, an
// unknown type).
std::optional<bool> decision_text_truncated(const std::string& decision_type,
                                            const std::string& text);

// Why a `laya` model would cut these labels, or "" when it reads them whole.
// `label_tokens[i]` counts the tokens of " " + labels[i].
std::string laya_label_overflow(const std::vector<std::string>& labels,
                                const std::vector<size_t>& label_tokens,
                                int64_t max_head_tokens);

// The /v1/systemone response to that body -> the /v1/classify shape
// {"labels": {label: probability}, "truncated": ...}, keeping the `top_k`
// highest when top_k > 0; `truncated` is left out when it has no value. Throws
// std::runtime_error unless the answer scores exactly the given labels.
nlohmann::json systemone_classify_response(const nlohmann::json& response,
                                           const nlohmann::json& labels,
                                           int top_k,
                                           std::optional<bool> truncated);

// llama-server caps an encoder decision model's slot at its trained window, so
// a larger context buys nothing.
int encoder_decision_ctx_size(int requested, int64_t trained_window);

// Which of -b and -ub lemond adds, and at what size, given the flags in
// `user_args`; and the longest input then accepted. Without flags the batch
// stops at 8192 tokens; -b / -ub raise it.
struct SingleBatchPlan {
    bool add_batch_size = false;
    bool add_ubatch_size = false;
    int batch_size = 0;
    int max_input_tokens = 0;
};

SingleBatchPlan plan_single_batch(int ctx_size, const std::vector<std::string>& user_args);

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int max_input_tokens);

// Why a model labelled `systemone` must not load, or "" when it may: hf_load, no
// decision head, or a decision type the pinned llama.cpp does not know. An empty
// `gguf_architecture` means lemond could not read the GGUF header.
std::string decision_model_load_refusal(const std::string& model_name, bool hf_load,
                                        const std::string& gguf_architecture,
                                        const std::string& gguf_decision_type);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
