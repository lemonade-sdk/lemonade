#include "lemon/routing_classifier_services.h"

#include "lemon/thinking_controls.h"

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace lemon {
namespace {

// Output cap for internal classifier chat calls (the `llm` router). The reply
// contract is a single small JSON object ({"model": ..., "rationale": ...});
// this bound keeps a misbehaving router model from streaming an essay.
constexpr int kClassifierChatMaxTokens = 256;

json llm_router_response_format() {
    // Use the portable JSON object contract; Lemonade validates the fields.
    return {{"type", "json_object"}};
}

void throw_if_error_response(const json& response, const std::string& context) {
    if (!response.is_object() || !response.contains("error")) {
        return;
    }
    std::string message = context + " failed";
    const json& error = response["error"];
    if (error.is_object() && error.contains("message") && error["message"].is_string()) {
        message += ": " + error["message"].get<std::string>();
    }
    throw std::runtime_error(message);
}

void ensure_model(const EnsureClassifierModelLoaded& ensure_loaded,
                  const std::string& model) {
    if (ensure_loaded) {
        ensure_loaded(model);
    }
}

std::map<std::string, double> scores_from_object(const json& obj) {
    std::map<std::string, double> scores;
    if (!obj.is_object()) {
        return scores;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().is_number()) {
            scores[it.key()] = it.value().get<double>();
        }
    }
    return scores;
}

std::map<std::string, double> scores_from_label_score_array(const json& arr) {
    std::map<std::string, double> scores;
    if (!arr.is_array()) {
        return scores;
    }
    for (const auto& item : arr) {
        if (!item.is_object() ||
            !item.contains("label") || !item["label"].is_string() ||
            !item.contains("score") || !item["score"].is_number()) {
            continue;
        }
        scores[item["label"].get<std::string>()] = item["score"].get<double>();
    }
    return scores;
}

std::map<std::string, double> parse_scores_payload(const json& payload) {
    if (payload.is_array()) {
        return scores_from_label_score_array(payload);
    }
    if (!payload.is_object()) {
        return {};
    }

    for (const char* key : {"labels", "scores", "classification", "classifications"}) {
        if (!payload.contains(key)) {
            continue;
        }
        if (payload[key].is_object()) {
            auto scores = scores_from_object(payload[key]);
            if (!scores.empty()) return scores;
        }
        if (payload[key].is_array()) {
            auto scores = scores_from_label_score_array(payload[key]);
            if (!scores.empty()) return scores;
        }
    }

    if (payload.contains("label") && payload["label"].is_string() &&
        payload.contains("score") && payload["score"].is_number()) {
        return {{payload["label"].get<std::string>(), payload["score"].get<double>()}};
    }

    if (payload.contains("data") && payload["data"].is_array()) {
        auto scores = scores_from_label_score_array(payload["data"]);
        if (!scores.empty()) return scores;
    }

    return scores_from_object(payload);
}

json parse_json_text(const std::string& text) {
    json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return parsed.is_discarded() ? json(nullptr) : parsed;
}

void validate_score_range(const std::map<std::string, double>& scores) {
    for (const auto& [label, score] : scores) {
        if (!std::isfinite(score) || score < 0.0 || score > 1.0) {
            throw std::runtime_error(
                "classifier score for label '" + label + "' must be in [0, 1]");
        }
    }
}

std::optional<std::string> try_extract_chat_text(const json& response) {
    if (response.is_object() && response.contains("choices") &&
        response["choices"].is_array() && !response["choices"].empty()) {
        const json& choice = response["choices"].front();
        if (choice.is_object()) {
            if (choice.contains("message") && choice["message"].is_object()) {
                const json& message = choice["message"];
                if (message.contains("content") && message["content"].is_string()) {
                    return message["content"].get<std::string>();
                }
            }
            if (choice.contains("text") && choice["text"].is_string()) {
                return choice["text"].get<std::string>();
            }
        }
    }

    if (response.is_object() && response.contains("content") &&
        response["content"].is_string()) {
        return response["content"].get<std::string>();
    }

    return std::nullopt;
}

// Concatenate the text of an OpenAI message `content` field (string or an array
// of typed parts), joining multiple text parts with newlines.
std::string collect_text_from_content(const json& content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    std::string text;
    if (content.is_array()) {
        for (const auto& part : content) {
            if (!part.is_object()) continue;
            const std::string type = part.value("type", std::string());
            // chat/completions uses "text"; the Responses API uses "input_text".
            if ((type == "text" || type == "input_text") &&
                part.contains("text") && part["text"].is_string()) {
                if (!text.empty()) text += "\n";
                text += part["text"].get<std::string>();
            }
        }
    }
    return text;
}

// True if an OpenAI message `content` array carries an image part.
bool content_has_image(const json& content) {
    if (!content.is_array()) return false;
    for (const auto& part : content) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string());
        // chat/completions uses "image_url"; the Responses API uses "input_image".
        if (type == "image_url" || type == "input_image") {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<float> parse_embedding_vector(const json& response) {
    throw_if_error_response(response, "embedding request");

    const json* embedding = nullptr;
    if (response.is_object() && response.contains("data") &&
        response["data"].is_array() && !response["data"].empty()) {
        const json& first = response["data"].front();
        if (first.is_object() && first.contains("embedding")) {
            embedding = &first["embedding"];
        }
    }
    if (!embedding && response.is_object() && response.contains("embedding")) {
        embedding = &response["embedding"];
    }
    if (!embedding || !embedding->is_array()) {
        throw std::runtime_error("embedding response did not contain an embedding array");
    }

    std::vector<float> result;
    result.reserve(embedding->size());
    for (const auto& value : *embedding) {
        if (!value.is_number()) {
            throw std::runtime_error("embedding response contained a non-numeric value");
        }
        result.push_back(value.get<float>());
    }
    if (result.empty()) {
        throw std::runtime_error("embedding response contained an empty embedding");
    }
    return result;
}

std::string extract_chat_text(const json& response) {
    throw_if_error_response(response, "chat completion request");

    if (auto text = try_extract_chat_text(response)) {
        return *text;
    }

    throw std::runtime_error("chat completion response did not contain text content");
}

std::map<std::string, double> parse_classifier_scores(const json& response) {
    throw_if_error_response(response, "classifier request");

    if (auto content = try_extract_chat_text(response)) {
        json parsed = parse_json_text(*content);
        auto scores = parse_scores_payload(parsed);
        if (!scores.empty()) {
            validate_score_range(scores);
            return scores;
        }
        throw std::runtime_error(
            "classifier chat content did not contain label scores");
    }

    auto scores = parse_scores_payload(response);
    if (!scores.empty()) {
        validate_score_range(scores);
        return scores;
    }

    throw std::runtime_error("classifier response did not contain label scores");
}

ClassifierServices make_classifier_services_from_router_calls(
    RouterJsonCall embeddings,
    RouterJsonCall chat_completion,
    EnsureClassifierModelLoaded ensure_loaded,
    RouterJsonCall classify,
    RouterModelTypeCall get_model_type) {
    ClassifierServices services;
    auto embeddings_call = std::make_shared<RouterJsonCall>(std::move(embeddings));
    auto chat_completion_call =
        std::make_shared<RouterJsonCall>(std::move(chat_completion));
    auto classify_call = std::make_shared<RouterJsonCall>(std::move(classify));

    services.embed = [embeddings_call,
                      ensure_loaded](const std::string& model,
                                     const std::string& text) -> std::vector<float> {
        if (!*embeddings_call) {
            throw std::runtime_error("Router embeddings call is not configured");
        }
        ensure_model(ensure_loaded, model);
        json request = {
            {"model", model},
            {"input", text},
        };
        return parse_embedding_vector((*embeddings_call)(request));
    };

    services.run_classifier = [chat_completion_call, classify_call, get_model_type,
                               ensure_loaded](const std::string& model,
                                              const std::string& input)
        -> std::map<std::string, double> {
        // Load the model BEFORE resolving its runtime type. The production
        // resolver (Router::get_model_type) reports ModelType::LLM until the
        // backend is loaded and alive, so resolving first would make a cold-start
        // request — the first after startup, eviction, or a backend restart —
        // pick the chat path for a model that only speaks /v1/classify,
        // recreating the unsupported-capability failure this routing exists to
        // avoid.
        ensure_model(ensure_loaded, model);

        // Models registered under ModelType::CLASSIFICATION (the onnxruntime
        // backend) speak /v1/classify natively — a real encoder classification
        // head, not an LLM asked to emit JSON. Route to it directly instead of
        // going through chat_completion, which such a model can't serve at all
        // (WrappedServer's default chat_completion() is an unsupported-capability
        // error for any backend that doesn't implement it).
        const bool use_classify_endpoint =
            classify_call && *classify_call && get_model_type &&
            get_model_type(model) == ModelType::CLASSIFICATION;

        if (use_classify_endpoint) {
            json request = {
                {"model", model},
                {"input", input},
            };
            return parse_classifier_scores((*classify_call)(request));
        }

        if (!*chat_completion_call) {
            throw std::runtime_error("Router chat_completion call is not configured");
        }

        json request = {
            {"model", model},
            {"stream", false},
            {"temperature", 0.0},
            {"messages", json::array({
                {
                    {"role", "system"},
                    {"content", "Classify the user input. Return only JSON mapping label names to numeric scores."},
                },
                {
                    {"role", "user"},
                    {"content", input},
                },
            })},
        };
        return parse_classifier_scores((*chat_completion_call)(request));
    };

    services.chat = [chat_completion_call,
                     ensure_loaded](const std::string& model,
                                    const std::string& prompt,
                                    const std::string& input) -> std::string {
        if (!*chat_completion_call) {
            throw std::runtime_error("Router chat_completion call is not configured");
        }
        ensure_model(ensure_loaded, model);
        // A deliberately constrained classifier invocation: deterministic
        // (temperature 0), non-streaming, thinking disabled through the same
        // cross-backend normalization as normal Lemonade requests (so e.g. a
        // Qwen3-style router doesn't emit <think> blocks or burn seconds
        // reasoning), and output tightly bounded — the reply contract is one
        // small JSON object, never long-form text.
        json request = {
            {"model", model},
            {"stream", false},
            {"temperature", 0.0},
            {"max_tokens", kClassifierChatMaxTokens},
            {"enable_thinking", false},
            {"response_format", llm_router_response_format()},
            {"messages", json::array({
                {{"role", "system"}, {"content", prompt}},
                {{"role", "user"}, {"content", input}},
            })},
        };
        normalize_thinking_controls(request);
        return extract_chat_text((*chat_completion_call)(request));
    };

    return services;
}

RouteContext build_route_context(const json& request_json, const std::string& model_name) {
    RouteContext ctx;
    ctx.params.model = model_name;

    auto append_line = [&ctx](const std::string& text) {
        if (!ctx.input.empty()) ctx.input += "\n";
        ctx.input += text;
    };

    if (request_json.contains("tools") && request_json["tools"].is_array() &&
        !request_json["tools"].empty()) {
        ctx.params.has_tools = true;
    }

    if (request_json.contains("messages") && request_json["messages"].is_array()) {
        const auto& messages = request_json["messages"];
        for (const auto& msg : messages) {
            if (msg.is_object() && msg.contains("content") && content_has_image(msg["content"])) {
                ctx.params.has_images = true;
                break;
            }
        }
        for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
            const auto& msg = messages[i];
            if (msg.is_object() && msg.value("role", std::string()) == "user" &&
                msg.contains("content")) {
                ctx.input = collect_text_from_content(msg["content"]);
                break;
            }
        }
    } else if (request_json.contains("prompt")) {
        const auto& prompt = request_json["prompt"];
        if (prompt.is_string()) {
            ctx.input = prompt.get<std::string>();
        } else if (prompt.is_array()) {
            for (const auto& part : prompt) {
                if (part.is_string()) {
                    append_line(part.get<std::string>());
                }
            }
        }
    } else if (request_json.contains("input")) {
        const auto& input = request_json["input"];
        if (input.is_string()) {
            ctx.input = input.get<std::string>();
        } else if (input.is_array()) {
            // Detect images anywhere in the input, mirroring how the chat path
            // scans every message.
            for (const auto& item : input) {
                if (!item.is_object()) continue;
                const json content =
                    item.contains("content") ? item["content"] : json::array({item});
                if (content_has_image(content)) {
                    ctx.params.has_images = true;
                    break;
                }
            }
            // Prefer the last user message's content, matching the chat path, so
            // earlier assistant/developer/system turns can't skew routing.
            bool found_user = false;
            for (int i = static_cast<int>(input.size()) - 1; i >= 0; --i) {
                const auto& item = input[i];
                if (item.is_object() &&
                    item.value("role", std::string()) == "user" &&
                    item.contains("content")) {
                    ctx.input = collect_text_from_content(item["content"]);
                    found_user = true;
                    break;
                }
            }
            // Fallback for role-less inputs (plain strings or bare content
            // parts): concatenate their text in order.
            if (!found_user) {
                for (const auto& item : input) {
                    if (item.is_string()) {
                        append_line(item.get<std::string>());
                    } else if (item.is_object() && !item.contains("role")) {
                        const json content = item.contains("content")
                                                 ? item["content"]
                                                 : json::array({item});
                        std::string part_text = collect_text_from_content(content);
                        if (!part_text.empty()) {
                            append_line(part_text);
                        }
                    }
                }
            }
        }
    }

    ctx.params.chars = ctx.input.size();

    if (request_json.contains("metadata") && request_json["metadata"].is_object()) {
        for (auto it = request_json["metadata"].begin();
             it != request_json["metadata"].end(); ++it) {
            if (it.value().is_string()) {
                ctx.metadata[it.key()] = it.value().get<std::string>();
            }
        }
    }

    return ctx;
}

} // namespace lemon
