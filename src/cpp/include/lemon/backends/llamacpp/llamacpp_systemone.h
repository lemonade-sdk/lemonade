#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

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
// {"labels": {label: probability}}, keeping the `top_k` highest when top_k > 0.
// Throws std::runtime_error unless the answer scores exactly the given labels.
nlohmann::json systemone_classify_response(const nlohmann::json& response,
                                           const nlohmann::json& labels,
                                           int top_k);

// An encoder decision model gets one physical batch as large as its context,
// and llama-server sizes the compute buffer for that batch even though it caps
// the slot at the trained window.
int encoder_decision_ctx_size(int requested, int64_t trained_window);

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int context_size);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
