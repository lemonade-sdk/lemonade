#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
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

// The /v1/systemone response to that body -> the /v1/classify shape
// {"labels": {label: probability}, "reports_truncation": true}, keeping the
// `top_k` highest when top_k > 0. Throws std::runtime_error unless the answer
// scores exactly the given labels.
nlohmann::json systemone_classify_response(const nlohmann::json& response,
                                           const nlohmann::json& labels,
                                           int top_k);

// An encoder decision model gets one physical batch as large as its context,
// and llama-server sizes the compute buffer for that batch even though it caps
// the slot at the trained window.
int encoder_decision_ctx_size(int requested, int64_t trained_window);

// Which of -b and -ub lemond adds at ctx_size, given the flags in `user_args`,
// and the longest input llama-server then accepts.
struct SingleBatchPlan {
    bool add_batch_size = false;
    bool add_ubatch_size = false;
    int max_input_tokens = 0;
};

SingleBatchPlan plan_single_batch(int ctx_size, const std::vector<std::string>& user_args);

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int max_input_tokens);

// Why a model labelled `systemone` must not load, or "" when it may. An empty
// `gguf_architecture` means lemond could not read the GGUF header.
std::string decision_model_load_refusal(const std::string& model_name, bool hf_load,
                                        const std::string& gguf_architecture,
                                        const std::string& gguf_decision_type);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
