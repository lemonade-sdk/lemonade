#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {
namespace {

constexpr const char* QUESTION_ID = "label";

// Decision models need a question; zero-shot labels come without one.
constexpr const char* CLASSIFY_INSTRUCTIONS = "Which category does this text belong to?";

}  // namespace

nlohmann::ordered_json build_systemone_classify_request(const std::string& text,
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

nlohmann::json systemone_classify_response(const nlohmann::json& response,
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
    return nlohmann::json{{"labels", std::move(scores)}};
}

int encoder_decision_ctx_size(int requested, int64_t trained_window) {
    if (trained_window <= 0 || trained_window > std::numeric_limits<int>::max()) {
        return requested;
    }
    if (requested <= 0 || requested > trained_window) {
        return static_cast<int>(trained_window);
    }
    return requested;
}

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int context_size) {
    if (!response.is_object() || !response.contains("error") || !response["error"].is_object()) {
        return response;
    }
    const nlohmann::json& error = response["error"];
    const std::string message = error.value("message", "");
    if (message.find("increase the physical batch size") == std::string::npos) {
        return response;
    }
    // llama-server words it "input (N tokens) is too large to process. ..."
    std::string input = "the input";
    const size_t end = message.find(" tokens)");
    if (message.rfind("input (", 0) == 0 && end != std::string::npos) {
        input = message.substr(0, end + 8);
    }
    return nlohmann::json{{"error", {
        {"message", input + " is longer than the context window of this decision model (" +
                        std::to_string(context_size) + " tokens)"},
        {"type", "invalid_request_error"},
        {"code", "context_length_exceeded"},
        {"status_code", 400},
    }}};
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
