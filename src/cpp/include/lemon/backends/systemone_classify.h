#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lemon {
namespace backends {
namespace systemone {

// A 400 error body for a /v1/classify request a decision model cannot answer.
nlohmann::json invalid_classify_request(const std::string& message);

// /v1/classify with `labels` -> a /v1/systemone body that asks one `choice`
// question whose options are those labels, in their order. Throws
// std::invalid_argument unless `labels` is a non-empty array of distinct,
// non-blank strings.
nlohmann::ordered_json build_classify_request(const std::string& text,
                                              const nlohmann::json& labels);

// The /v1/systemone response to that body -> the /v1/classify shape
// {"labels": {label: probability}, "truncated": ...}, keeping the `top_k`
// highest when top_k > 0; `truncated` is left out when it has no value. Throws
// std::runtime_error unless the answer scores exactly the given labels.
nlohmann::json classify_response(const nlohmann::json& response,
                                 const nlohmann::json& labels,
                                 int top_k,
                                 std::optional<bool> truncated);

}  // namespace systemone
}  // namespace backends
}  // namespace lemon
