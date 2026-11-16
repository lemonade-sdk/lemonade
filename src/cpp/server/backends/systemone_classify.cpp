#include "lemon/backends/systemone_classify.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {
namespace systemone {
namespace {

constexpr const char* QUESTION_ID = "label";

// Decision models need a question; zero-shot labels come without one.
constexpr const char* CLASSIFY_INSTRUCTIONS = "Which category does this text belong to?";

}  // namespace

nlohmann::json invalid_decision_request(const std::string& message) {
    return nlohmann::json{{"error", {
        {"message", message},
        {"type", "invalid_request_error"},
        {"status_code", 400},
    }}};
}

nlohmann::ordered_json build_classify_request(const std::string& text,
                                              const nlohmann::json& labels) {
    if (!labels.is_array() || labels.empty()) {
        throw std::invalid_argument("\"labels\" must be a non-empty array of strings");
    }
    nlohmann::ordered_json criteria = nlohmann::ordered_json::object();
    for (const auto& label : labels) {
        if (!label.is_string()) {
            throw std::invalid_argument("\"labels\" must be a non-empty array of strings");
        }
        const std::string value = label.get<std::string>();
        if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
            throw std::invalid_argument("labels must not be empty or whitespace-only");
        }
        if (criteria.contains(value)) {
            throw std::invalid_argument("duplicate label: '" + value + "'");
        }
        criteria[value] = nullptr;
    }
    return nlohmann::ordered_json{
        {"state", text},
        {"questions", {
            {QUESTION_ID, {
                {"type", "choice"},
                {"instructions", CLASSIFY_INSTRUCTIONS},
                {"criteria", std::move(criteria)},
            }},
        }},
    };
}

nlohmann::json classify_response(const nlohmann::json& response,
                                 const nlohmann::json& labels,
                                 int top_k) {
    const auto unexpected = std::runtime_error(
        "decision model response did not score the requested labels");
    if (!response.is_object() || !response.contains("answers") ||
        !response["answers"].is_object() || !response["answers"].contains(QUESTION_ID)) {
        throw unexpected;
    }
    const nlohmann::json& answer = response["answers"][QUESTION_ID];
    if (!answer.is_object() || !answer.contains("probabilities") ||
        !answer["probabilities"].is_object()) {
        throw unexpected;
    }
    const nlohmann::json& probabilities = answer["probabilities"];

    std::set<std::string> requested;
    for (const auto& label : labels) requested.insert(label.get<std::string>());
    if (probabilities.size() != requested.size()) {
        throw unexpected;
    }

    std::vector<std::pair<std::string, double>> ranked;
    for (const auto& [label, value] : probabilities.items()) {
        if (!requested.count(label) || !value.is_number()) {
            throw unexpected;
        }
        const double probability = value.get<double>();
        if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
            throw unexpected;
        }
        ranked.emplace_back(label, probability);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (top_k > 0 && static_cast<size_t>(top_k) < ranked.size()) {
        ranked.resize(static_cast<size_t>(top_k));
    }

    nlohmann::json scores = nlohmann::json::object();
    for (const auto& [label, probability] : ranked) scores[label] = probability;
    nlohmann::json out = {{"labels", std::move(scores)}};
    if (answer.contains("truncated") && answer["truncated"].is_boolean()) {
        out["truncated"] = answer["truncated"];
    }
    return out;
}

void set_answers_truncated(nlohmann::ordered_json& response, bool truncated) {
    if (!response.is_object() || !response.contains("answers") ||
        !response["answers"].is_object()) {
        return;
    }
    for (auto& answer : response["answers"]) {
        if (answer.is_object()) answer["truncated"] = truncated;
    }
}

std::optional<nlohmann::json> context_length_refusal(const nlohmann::json& response) {
    if (!response.is_object() || !response.contains("answers") ||
        !response["answers"].is_object() || response["answers"].empty()) {
        return std::nullopt;
    }
    std::string message;
    for (const auto& answer : response["answers"]) {
        if (!answer.is_object() || !answer.contains("error") ||
            answer["error"] != "max_length_exceeded") {
            return std::nullopt;
        }
        if (message.empty() && answer.contains("message") && answer["message"].is_string()) {
            message = answer["message"].get<std::string>();
        }
    }
    return nlohmann::json{{"error", {
        {"message", message.empty() ? "the text is longer than this decision model accepts"
                                    : message},
        {"type", "invalid_request_error"},
        {"code", "context_length_exceeded"},
        {"status_code", 400},
    }}};
}

}  // namespace systemone
}  // namespace backends
}  // namespace lemon
