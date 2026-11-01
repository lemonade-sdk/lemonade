#include "lemon/backends/llamacpp/llamacpp_systemone.h"
#include "lemon/llamacpp_architectures_generated.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
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

// An encoder reads each input in one physical batch, whose memory and time grow
// with the square of its size, so a large context alone must not set it.
constexpr int ENCODER_DEFAULT_BATCH = 8192;

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

namespace {

using ordered_json = nlohmann::ordered_json;

// OpenAI's limits. llama-server also refuses more than 10 score levels.
constexpr size_t DECISIONS_MAX_QUESTIONS = 400;
constexpr size_t DECISIONS_MAX_CHOICES = 255;
constexpr size_t DECISIONS_MAX_LEVELS = 10;

void allow_keys(const ordered_json& object, std::initializer_list<std::string_view> keys,
                const std::string& path) {
    for (const auto& item : object.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
            throw std::invalid_argument(path + "." + item.key() + " is not supported");
        }
    }
}

const ordered_json& member(const ordered_json& object, const char* key, const std::string& path) {
    if (!object.contains(key)) {
        throw std::invalid_argument(path + "." + key + " is required");
    }
    return object.at(key);
}

std::string text_member(const ordered_json& object, const char* key, const std::string& path) {
    const ordered_json& value = member(object, key, path);
    if (!value.is_string()) {
        throw std::invalid_argument(path + "." + key + " must be a string");
    }
    return value.get<std::string>();
}

std::string join(const std::vector<std::string>& parts, const std::string& separator) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += separator;
        out += parts[i];
    }
    return out;
}

std::string question_id(size_t index) { return "q" + std::to_string(index); }

// /v1/systemone keys an option by its text, so a string and a boolean with the
// same text, distinct OpenAI choices, would be one option.
std::string choice_key(const ordered_json& value) {
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return value.get<std::string>();
}

// Joined as vLLM joins them: the parts of a message with a new line, the messages
// with a blank line. One input_json part on its own stays JSON for the template.
ordered_json decisions_state(const ordered_json& input, ordered_json& images) {
    if (input.is_string()) {
        return input;
    }
    if (!input.is_array() || input.empty()) {
        throw std::invalid_argument(
            "\"input\" must be a string or a non-empty array of user messages");
    }
    std::vector<std::string> messages;
    std::vector<ordered_json> json_parts;
    size_t n_parts = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const ordered_json& message = input[i];
        const std::string path = "input[" + std::to_string(i) + "]";
        if (!message.is_object()) {
            throw std::invalid_argument(path + " must be a message object");
        }
        allow_keys(message, {"type", "role", "content"}, path);
        if ((message.contains("type") && message.at("type") != "message") ||
            !message.contains("role") || message.at("role") != "user") {
            throw std::invalid_argument(path + ": only user messages are supported");
        }
        const ordered_json& content = member(message, "content", path);
        std::vector<std::string> texts;
        if (content.is_string()) {
            texts.push_back(content.get<std::string>());
        } else if (content.is_array()) {
            for (size_t j = 0; j < content.size(); ++j) {
                const ordered_json& part = content[j];
                const std::string part_path = path + ".content[" + std::to_string(j) + "]";
                if (!part.is_object()) {
                    throw std::invalid_argument(part_path + " must be an object");
                }
                const std::string type = text_member(part, "type", part_path);
                if (type == "input_text") {
                    allow_keys(part, {"type", "text", "prompt_cache_breakpoint"}, part_path);
                    texts.push_back(text_member(part, "text", part_path));
                } else if (type == "input_image") {
                    allow_keys(part, {"type", "image_url", "detail", "prompt_cache_breakpoint"},
                               part_path);
                    const std::string url = text_member(part, "image_url", part_path);
                    if (url.rfind("data:", 0) != 0) {
                        throw std::invalid_argument(
                            part_path + ".image_url must be a data URL; lemond does not "
                            "download images");
                    }
                    images.push_back(url);
                } else if (type == "input_json") {
                    allow_keys(part, {"type", "data"}, part_path);
                    const ordered_json& data = member(part, "data", part_path);
                    if (!data.is_object() && !data.is_array()) {
                        throw std::invalid_argument(part_path +
                                                    ".data must be an object or an array");
                    }
                    json_parts.push_back(data);
                    texts.push_back(data.dump());
                } else {
                    throw std::invalid_argument(part_path + ".type '" + type +
                                                "' is not supported");
                }
            }
        } else {
            throw std::invalid_argument(path + ".content must be a string or an array of parts");
        }
        n_parts += texts.size();
        if (!texts.empty()) messages.push_back(join(texts, "\n"));
    }
    if (n_parts == 1 && json_parts.size() == 1) {
        return json_parts.front();
    }
    if (messages.empty()) {
        if (images.empty()) {
            throw std::invalid_argument("\"input\" has no text, JSON or image to judge");
        }
        return nullptr;
    }
    return join(messages, "\n\n");
}

ordered_json question_instructions(const ordered_json& question, const std::string& path) {
    const ordered_json& instructions = member(question, "instructions", path);
    if (instructions.is_string()) {
        return instructions;
    }
    if (!instructions.is_array() || instructions.empty()) {
        throw std::invalid_argument(path + ".instructions must be a string or a non-empty "
                                           "array of input_text parts");
    }
    std::vector<std::string> texts;
    for (size_t j = 0; j < instructions.size(); ++j) {
        const ordered_json& part = instructions[j];
        const std::string part_path = path + ".instructions[" + std::to_string(j) + "]";
        if (!part.is_object() || !part.contains("type") || part.at("type") != "input_text") {
            throw std::invalid_argument(part_path + ": /v1/systemone has no images or JSON per "
                                                    "question, so instructions take input_text "
                                                    "parts only");
        }
        allow_keys(part, {"type", "text", "prompt_cache_breakpoint"}, part_path);
        texts.push_back(text_member(part, "text", part_path));
    }
    return join(texts, "\n");
}

std::runtime_error unanswered() {
    return std::runtime_error("decision model response did not answer the requested questions");
}

double answer_number(const ordered_json& object, const std::string& key) {
    if (!object.contains(key) || !object.at(key).is_number()) {
        throw unanswered();
    }
    const double value = object.at(key).get<double>();
    if (!std::isfinite(value)) {
        throw unanswered();
    }
    return value;
}

}  // namespace

nlohmann::ordered_json build_systemone_decisions_request(const nlohmann::ordered_json& request) {
    if (!request.is_object() || !request.contains("input")) {
        throw std::invalid_argument("\"input\" is required");
    }
    ordered_json images = ordered_json::array();
    ordered_json state = decisions_state(request.at("input"), images);

    const ordered_json questions = request.value("questions", ordered_json());
    if (!questions.is_array() || questions.empty() ||
        questions.size() > DECISIONS_MAX_QUESTIONS) {
        throw std::invalid_argument("\"questions\" must be an array of 1 to " +
                                    std::to_string(DECISIONS_MAX_QUESTIONS) + " questions");
    }
    ordered_json out = ordered_json::object();
    std::set<std::string> names;
    for (size_t i = 0; i < questions.size(); ++i) {
        const ordered_json& q = questions[i];
        const std::string path = "questions[" + std::to_string(i) + "]";
        if (!q.is_object()) {
            throw std::invalid_argument(path + " must be an object");
        }
        const std::string type = text_member(q, "type", path);
        if (type == "predicate") {
            allow_keys(q, {"type", "name", "instructions"}, path);
        } else if (type == "choice") {
            allow_keys(q, {"type", "name", "instructions", "choices"}, path);
        } else if (type == "score") {
            allow_keys(q, {"type", "name", "instructions", "levels"}, path);
        } else {
            throw std::invalid_argument(path + ".type must be predicate, choice or score");
        }
        if (q.contains("name")) {
            const std::string name = text_member(q, "name", path);
            if (!names.insert(name).second) {
                throw std::invalid_argument(path + ".name '" + name + "' is used twice");
            }
        }

        ordered_json question = {{"type", type == "predicate" ? "noul" : type},
                                 {"instructions", question_instructions(q, path)}};
        if (type == "choice") {
            const ordered_json& choices = member(q, "choices", path);
            if (!choices.is_array() || choices.size() < 2 ||
                choices.size() > DECISIONS_MAX_CHOICES) {
                throw std::invalid_argument(path + ".choices must be an array of 2 to " +
                                            std::to_string(DECISIONS_MAX_CHOICES) + " choices");
            }
            ordered_json criteria = ordered_json::object();
            for (size_t j = 0; j < choices.size(); ++j) {
                const ordered_json& choice = choices[j];
                const std::string choice_path = path + ".choices[" + std::to_string(j) + "]";
                if (!choice.is_object()) {
                    throw std::invalid_argument(choice_path + " must be an object");
                }
                allow_keys(choice, {"value", "description"}, choice_path);
                const ordered_json& value = member(choice, "value", choice_path);
                if (!value.is_string() && !value.is_boolean()) {
                    throw std::invalid_argument(choice_path + ".value must be a string or a boolean");
                }
                const std::string key = choice_key(value);
                if (key.empty()) {
                    throw std::invalid_argument(choice_path + ".value must not be empty");
                }
                if (criteria.contains(key)) {
                    throw std::invalid_argument(
                        choice_path + ".value '" + key + "' is used twice; a string and a "
                        "boolean with the same text are one option here");
                }
                criteria[key] = choice.contains("description")
                                    ? ordered_json(text_member(choice, "description", choice_path))
                                    : ordered_json(nullptr);
            }
            question["criteria"] = std::move(criteria);
        } else if (type == "score") {
            const ordered_json& levels = member(q, "levels", path);
            if (!levels.is_array() || levels.size() < 2 || levels.size() > DECISIONS_MAX_LEVELS) {
                throw std::invalid_argument(path + ".levels must be an array of 2 to " +
                                            std::to_string(DECISIONS_MAX_LEVELS) + " levels");
            }
            ordered_json criteria = ordered_json::array();
            for (size_t j = 0; j < levels.size(); ++j) {
                const ordered_json& level = levels[j];
                const std::string level_path = path + ".levels[" + std::to_string(j) + "]";
                if (!level.is_object()) {
                    throw std::invalid_argument(level_path + " must be an object");
                }
                allow_keys(level, {"label", "description"}, level_path);
                const std::string label = text_member(level, "label", level_path);
                if (label.empty()) {
                    throw std::invalid_argument(level_path + ".label must not be empty");
                }
                criteria.push_back(level.contains("description")
                                       ? label + ": " + text_member(level, "description", level_path)
                                       : label);
            }
            question["criteria"] = std::move(criteria);
        }
        out[question_id(i)] = std::move(question);
    }

    ordered_json body = {{"state", std::move(state)}, {"questions", std::move(out)}};
    if (!images.empty()) {
        body["images"] = std::move(images);
    }
    return body;
}

std::vector<std::vector<std::string>> laya_option_texts(const nlohmann::ordered_json& body) {
    std::vector<std::vector<std::string>> texts;
    for (const auto& entry : body.at("questions").items()) {
        const ordered_json& question = entry.value();
        const std::string type = question.at("type").get<std::string>();
        std::vector<std::string> options;
        if (type == "choice") {
            for (const auto& [key, description] : question.at("criteria").items()) {
                const bool described = description.is_string() && !description.get<std::string>().empty();
                options.push_back(described ? key + ": " + description.get<std::string>() : key);
            }
        } else if (type == "score") {
            const ordered_json& levels = question.at("criteria");
            for (size_t i = 0; i < levels.size(); ++i) {
                options.push_back("level " + std::to_string(i) + ": " + levels[i].get<std::string>());
            }
        }
        if (!options.empty()) texts.push_back(std::move(options));
    }
    return texts;
}

nlohmann::ordered_json systemone_decisions_response(const nlohmann::ordered_json& response,
                                                   const nlohmann::ordered_json& request,
                                                   const std::string& model) {
    if (!response.is_object() || !response.contains("answers") ||
        !response.at("answers").is_object()) {
        throw unanswered();
    }
    const ordered_json& answers = response.at("answers");
    const ordered_json& questions = request.at("questions");
    ordered_json out = ordered_json::array();
    for (size_t i = 0; i < questions.size(); ++i) {
        const ordered_json& question = questions[i];
        const std::string id = question_id(i);
        if (!answers.contains(id) || !answers.at(id).is_object()) {
            throw unanswered();
        }
        const ordered_json& answer = answers.at(id);
        const std::string type = question.at("type").get<std::string>();
        ordered_json item = {{"type", type},
                             {"name", question.value("name", ordered_json(nullptr))}};
        if (type == "predicate") {
            item["probability"] = answer_number(answer, "noul");
            out.push_back(std::move(item));
            continue;
        }
        if (!answer.contains("probabilities") || !answer.at("probabilities").is_object()) {
            throw unanswered();
        }
        const ordered_json& probabilities = answer.at("probabilities");
        ordered_json listed = ordered_json::array();
        if (type == "choice") {
            const ordered_json& choices = question.at("choices");
            if (probabilities.size() != choices.size()) {
                throw unanswered();
            }
            const std::string chosen = answer.contains("choice") && answer.at("choice").is_string()
                                           ? answer.at("choice").get<std::string>()
                                           : "";
            ordered_json choice;
            for (const auto& option : choices) {
                const std::string key = choice_key(option.at("value"));
                listed.push_back({{"value", option.at("value")},
                                  {"probability", answer_number(probabilities, key)}});
                if (key == chosen) choice = option.at("value");
            }
            if (choice.is_null()) {
                throw unanswered();
            }
            item["choice"] = std::move(choice);
        } else {
            const ordered_json& levels = question.at("levels");
            if (probabilities.size() != levels.size()) {
                throw unanswered();
            }
            for (size_t j = 0; j < levels.size(); ++j) {
                listed.push_back({{"value", j},
                                  {"label", levels[j].at("label")},
                                  {"probability", answer_number(probabilities, std::to_string(j))}});
            }
            item["score"] = answer_number(answer, "score");
        }
        item["probabilities"] = std::move(listed);
        item["confidence"] = answer_number(answer, "confidence");
        out.push_back(std::move(item));
    }

    int64_t input_tokens = 0;
    if (response.contains("usage") && response.at("usage").is_object() &&
        response.at("usage").contains("input_tokens") &&
        response.at("usage").at("input_tokens").is_number_integer()) {
        input_tokens = response.at("usage").at("input_tokens").get<int64_t>();
    }
    return ordered_json{
        {"model", model},
        {"answers", std::move(out)},
        {"usage", {
            {"input_tokens", input_tokens},
            {"input_tokens_details", {{"cached_tokens", 0}, {"cache_write_tokens", 0}}},
            {"output_tokens", 0},
            {"output_tokens_details", {{"reasoning_tokens", 0}}},
            {"total_tokens", input_tokens},
        }},
    };
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
    // A size the user gave one flag also sizes the flag lemond adds.
    const std::optional<int> asked = batch.value ? batch.value : ubatch.value;
    plan.batch_size = std::min(ctx_size, asked.value_or(ENCODER_DEFAULT_BATCH));
    // llama.cpp caps -ub at -b, and an encoder's embedding mode caps -b at -ub.
    plan.max_input_tokens = std::min({ctx_size, batch.value.value_or(plan.batch_size),
                                      ubatch.value.value_or(plan.batch_size)});
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
    if (gguf_architecture.empty()) {
        return "";
    }
    if (gguf_decision_type.empty()) {
        return "model '" + model_name + "' is labelled 'systemone', but its GGUF has no "
               "decision head ('" + gguf_architecture + ".decision.type'), so llama-server "
               "cannot answer /v1/systemone for it. Register it without the 'classification' "
               "and 'systemone' labels.";
    }
    // Refused here, before the Router evicts every model to retry a load that
    // cannot succeed.
    if (!llamacpp_decision_type_is_supported_by_all_backends(gguf_decision_type)) {
        return "model '" + model_name + "' has decision type '" + gguf_decision_type +
               "', which the llama.cpp that lemond runs does not support, so llama-server "
               "cannot load it. Use a GGUF made for upstream llama.cpp.";
    }
    return "";
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
