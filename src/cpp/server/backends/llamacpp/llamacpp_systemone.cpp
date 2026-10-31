#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
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

// llama.cpp server-decision.cpp: h2o-lightning keeps this many tokens of a
// state; laya keeps this many tokens of each option, and leaves this many
// tokens of its head to the question.
constexpr size_t H2O_MAX_STATE_TOKENS = 32000;
constexpr size_t LAYA_MAX_OPTION_TOKENS = 48;
constexpr size_t LAYA_QUESTION_RESERVE = 16;

struct BatchFlag {
    bool present = false;
    std::optional<int> value;
};

std::optional<int> positive_int(const std::string& text) {
    if (text.empty() || text.size() > 9 ||
        text.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    const int value = std::stoi(text);
    if (value <= 0) return std::nullopt;
    return value;
}

BatchFlag find_batch_flag(const std::vector<std::string>& args, const std::string& short_name,
                          const std::string& long_name) {
    BatchFlag flag;
    for (size_t i = 0; i < args.size(); ++i) {
        for (const std::string& name : {short_name, long_name}) {
            if (args[i] == name) {
                flag.present = true;
                flag.value = i + 1 < args.size() ? positive_int(args[i + 1]) : std::nullopt;
            } else if (args[i].rfind(name + "=", 0) == 0) {
                flag.present = true;
                flag.value = positive_int(args[i].substr(name.size() + 1));
            }
        }
    }
    return flag;
}

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

std::optional<bool> decision_text_truncated(const std::string& decision_type,
                                            const std::string& text) {
    // server-decision.cpp has no cut step for these types: they refuse a text
    // that does not fit. A type missing here may cut, so it reports nothing.
    static const std::set<std::string> refuse_long_text = {
        "laya", "openjev", "lev", "kev", "nimble", "clef", "pplx-decider",
        "vllm-decision2", "vllm-d3", "lfm2-d1", "granite-switch"};
    if (decision_type == "h2o-lightning") {
        // A byte-level BPE never has more tokens than bytes.
        if (text.size() <= H2O_MAX_STATE_TOKENS) return false;
        return std::nullopt;
    }
    if (refuse_long_text.count(decision_type) > 0) return false;
    return std::nullopt;
}

std::string laya_label_overflow(const std::vector<std::string>& labels,
                                const std::vector<size_t>& label_tokens,
                                int64_t max_head_tokens) {
    if (labels.empty() || labels.size() != label_tokens.size() || max_head_tokens <= 0) {
        return "";
    }
    const size_t max_head = static_cast<size_t>(max_head_tokens);
    // Each option is its marker token plus the label's tokens.
    size_t total = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (label_tokens[i] > LAYA_MAX_OPTION_TOKENS) {
            return "label '" + labels[i] + "' is " + std::to_string(label_tokens[i]) +
                   " tokens long; this decision model reads at most " +
                   std::to_string(LAYA_MAX_OPTION_TOKENS) + " tokens of each label";
        }
        total += 1 + label_tokens[i];
    }
    if (total + LAYA_QUESTION_RESERVE <= max_head) return "";
    const size_t room = max_head - std::min(max_head, LAYA_QUESTION_RESERVE);
    const size_t per_option = std::max<size_t>(4, room / labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        if (1 + label_tokens[i] > per_option) {
            return "the labels take " + std::to_string(total) +
                   " tokens with one marker token each; this decision model reads at most " +
                   std::to_string(room) + " (max_head_tokens " + std::to_string(max_head) +
                   " minus " + std::to_string(LAYA_QUESTION_RESERVE) + ")";
        }
    }
    return "";
}

nlohmann::json systemone_classify_response(const nlohmann::json& response,
                                           const nlohmann::json& labels,
                                           int top_k,
                                           std::optional<bool> truncated) {
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
    if (truncated.has_value()) out["truncated"] = *truncated;
    return out;
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

SingleBatchPlan plan_single_batch(int ctx_size, const std::vector<std::string>& user_args) {
    const BatchFlag batch = find_batch_flag(user_args, "-b", "--batch-size");
    const BatchFlag ubatch = find_batch_flag(user_args, "-ub", "--ubatch-size");
    SingleBatchPlan plan;
    plan.add_batch_size = !batch.present;
    plan.add_ubatch_size = !ubatch.present;
    // llama.cpp caps -ub at -b, and an encoder's embedding mode caps -b at -ub.
    plan.max_input_tokens = std::min(
        {ctx_size, batch.value.value_or(ctx_size), ubatch.value.value_or(ctx_size)});
    return plan;
}

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int max_input_tokens) {
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
        {"message", input + " is longer than this decision model accepts (" +
                        std::to_string(max_input_tokens) + " tokens)"},
        {"type", "invalid_request_error"},
        {"code", "context_length_exceeded"},
        {"status_code", 400},
    }}};
}

std::string decision_model_load_refusal(const std::string& model_name, bool hf_load,
                                        const std::string& gguf_architecture,
                                        const std::string& gguf_decision_type) {
    // With -hf, llama-server fetches the GGUF itself, so lemond never reads the
    // header that tells it to size an encoder's batch and context.
    if (hf_load) {
        return "model '" + model_name + "' is labelled 'systemone' and sets hf_load. lemond "
               "must read a decision model's GGUF header to configure it, so remove hf_load.";
    }
    // An unread header cannot be checked here; a GGUF without a decision head
    // then gets a 501 from llama-server on /v1/systemone.
    if (gguf_architecture.empty() || !gguf_decision_type.empty()) {
        return "";
    }
    return "model '" + model_name + "' is labelled 'systemone', but its GGUF has no "
           "decision head ('" + gguf_architecture + ".decision.type'), so llama-server "
           "cannot answer /v1/systemone for it. Register it without the 'classification' "
           "and 'systemone' labels.";
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
