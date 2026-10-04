#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <algorithm>
#include <cmath>
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

nlohmann::json build_systemone_classify_request(const std::string& text,
                                                const nlohmann::json& labels) {
    if (!labels.is_array() || labels.empty()) {
        throw std::invalid_argument("\"labels\" must be a non-empty array of strings");
    }
    nlohmann::json criteria = nlohmann::json::object();
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
    return nlohmann::json{
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

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
