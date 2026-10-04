#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace lemon {
namespace backends {
namespace llamacpp {

// /v1/classify with `labels` -> a /v1/systemone body that asks one `choice`
// question whose options are those labels. Throws std::invalid_argument unless
// `labels` is a non-empty array of distinct, non-blank strings.
nlohmann::json build_systemone_classify_request(const std::string& text,
                                                const nlohmann::json& labels);

// The /v1/systemone response to that body -> the /v1/classify shape
// {"labels": {label: probability}}, keeping the `top_k` highest when top_k > 0.
// Throws std::runtime_error unless the answer scores exactly the given labels.
nlohmann::json systemone_classify_response(const nlohmann::json& response,
                                           const nlohmann::json& labels,
                                           int top_k);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
