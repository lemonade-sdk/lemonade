#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lemon {
namespace backends {
namespace systemone {

// A 400 error body for a /v1/classify or /v1/systemone request a decision model
// cannot answer.
nlohmann::json invalid_decision_request(const std::string& message);

// /v1/classify with `labels` -> a /v1/systemone body that asks one `choice`
// question whose options are those labels, in their order. Throws
// std::invalid_argument unless `labels` is a non-empty array of distinct,
// non-blank strings.
nlohmann::ordered_json build_classify_request(const std::string& text,
                                              const nlohmann::json& labels);

// The /v1/systemone response to that body -> the /v1/classify shape
// {"labels": {label: probability}, "truncated": ...}, keeping the `top_k`
// highest when top_k > 0; `truncated` comes from the answer when it has one.
// Throws std::runtime_error unless the answer scores exactly the given labels.
nlohmann::json classify_response(const nlohmann::json& response,
                                 const nlohmann::json& labels,
                                 int top_k);

// Sets "truncated" on each answer of a /v1/systemone response.
void set_answers_truncated(nlohmann::ordered_json& response, bool truncated);

// A /v1/systemone response in which FLM refused every question because it did
// not fit the model's window (`"error": "max_length_exceeded"` inside a 200
// answer) -> the 400 context_length_exceeded body that llama.cpp gives for the
// same case. std::nullopt when at least one question was answered.
std::optional<nlohmann::json> context_length_refusal(const nlohmann::json& response);

}  // namespace systemone
}  // namespace backends
}  // namespace lemon
