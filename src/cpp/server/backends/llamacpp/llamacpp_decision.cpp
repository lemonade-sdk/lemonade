#include "lemon/backends/llamacpp/llamacpp_decision.h"

#include "lemon/auto_tune.h"
#include "lemon/backends/llamacpp/llamacpp_systemone.h"
#include "lemon/backends/systemone_classify.h"
#include "lemon/classification_labels.h"
#include "lemon/error_types.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {

using json = nlohmann::json;

std::optional<DecisionLaunch> plan_decision_launch(const std::string& model_name, const ModelInfo& model_info,
                                                   int ctx_size, const std::string& custom_args) {
    const bool rule_head = is_rule_head_router_model(model_info);
    const bool encoder_decision_model = is_encoder_decision_model(model_info);
    if (!rule_head && !encoder_decision_model) {
        return std::nullopt;
    }
    if (rule_head && model_info.resolved_path("rulehead").empty()) {
        throw std::runtime_error(missing_rule_head_message(model_name, model_info.checkpoint("rulehead")));
    }
    if (encoder_decision_model) {
        const int capped = encoder_decision_ctx_size(ctx_size, model_info.max_context_window);
        if (capped != ctx_size) {
            LOG(INFO, "LlamaCpp") << "Encoder decision model: using its trained context of " << capped
                                  << " tokens instead of " << ctx_size << std::endl;
            ctx_size = capped;
        }
    }
    if (rule_head) {
        const int window = rule_head_ctx_size(ctx_size);
        if (window != ctx_size) {
            LOG(INFO, "LlamaCpp") << "Prompt-router encoder: ctx_size " << ctx_size
                                  << " means its default window of " << window << " tokens" << std::endl;
            ctx_size = window;
        }
    }
    const SingleBatchPlan batch = plan_single_batch(ctx_size, utils::parse_custom_args(custom_args));
    if (batch.max_input_tokens < ctx_size) {
        LOG(INFO, "LlamaCpp") << "Encoder decision model reads up to " << batch.max_input_tokens
                              << " tokens per input (context " << ctx_size
                              << "); raise it with -b and -ub in llamacpp_args" << std::endl;
    } else {
        LOG(INFO, "LlamaCpp") << "Encoder decision model, batch sizes cover the context (" << ctx_size
                              << " tokens)" << std::endl;
    }
    return DecisionLaunch{rule_head, ctx_size, batch.add_batch_size, batch.add_ubatch_size,
                          batch.batch_size, batch.max_input_tokens};
}

DecisionRuntime::DecisionRuntime(const std::string& model_name, const ModelInfo& model_info)
    : model_name_(model_name),
      decision_model_(has_label(model_info.labels, "systemone")),
      decision_type_(model_info.gguf.decision_type),
      decision_max_head_tokens_(model_info.gguf.decision_max_head_tokens) {
    if (decision_model_ && model_info.gguf.architecture.empty()) {
        LOG(WARNING, "LlamaCpp") << "Could not read the GGUF header of decision model '" << model_name
                                 << "', so if it is an encoder it keeps llama-server's default batch sizes"
                                 << std::endl;
    }
    if (!is_rule_head_router_model(model_info)) {
        return;
    }
    const std::string rule_head_path = model_info.resolved_path("rulehead");
    if (rule_head_path.empty()) {
        throw std::runtime_error(missing_rule_head_message(model_name, model_info.checkpoint("rulehead")));
    }
    std::ifstream in(utils::path_from_utf8(rule_head_path), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        throw std::runtime_error("Could not read the rule head file " + rule_head_path);
    }
    rule_head_ = std::make_shared<const RuleHead>(parse_rule_head(bytes, model_info.gguf.embedding_length));
    LOG(INFO, "LlamaCpp") << "Prompt-router encoder: lemond applies its rule head (" << rule_head_->proj << "x"
                          << rule_head_->hidden << ") to llama-server's token vectors" << std::endl;
}

json DecisionRuntime::unsupported_decision_request(const std::string& operation) const {
    return ErrorResponse::from_exception(UnsupportedOperationException(
        operation, "llama.cpp model '" + model_name_ +
                       "', which is not a SystemOne decision model"));
}

json DecisionRuntime::classify(const DecisionTransport& transport, const json& request) {
    if (rule_head_) {
        return classify_with_rule_head(transport, request);
    }
    if (!decision_model_) {
        return unsupported_decision_request("Classification");
    }
    const json text = request.contains("text") ? request["text"] : request.value("input", json());
    if (!text.is_string()) {
        return systemone::invalid_decision_request("Missing 'input' (or 'text') string in classify request");
    }
    if (!request.contains("labels") || request["labels"].is_null()) {
        return systemone::invalid_decision_request(
            "this model scores a label list supplied per request; include a \"labels\" "
            "array in the request body");
    }

    nlohmann::ordered_json body;
    try {
        body = systemone::build_classify_request(text.get<std::string>(), request["labels"]);
    } catch (const std::invalid_argument& e) {
        return systemone::invalid_decision_request(e.what());
    }

    const json response(decide(transport, body));
    if (response.contains("error")) {
        return response;
    }
    const int top_k = request.contains("top_k") && request["top_k"].is_number_integer()
                          ? request["top_k"].get<int>()
                          : 0;
    try {
        return systemone::classify_response(response, request["labels"], top_k);
    } catch (const std::exception& e) {
        return ErrorResponse::create(e.what(), ErrorType::BACKEND_ERROR);
    }
}

json DecisionRuntime::classify_with_rule_head(const DecisionTransport& transport, const json& request) {
    const json text = request.contains("text") ? request["text"] : request.value("input", json());
    if (!text.is_string()) {
        return systemone::invalid_decision_request("Missing 'input' (or 'text') string in classify request");
    }
    if (!request.contains("labels") || !request["labels"].is_array() || request["labels"].empty()) {
        return systemone::invalid_decision_request(
            "this model scores a label list supplied per request; include a non-empty "
            "\"labels\" array in the request body");
    }
    std::vector<std::string> labels;
    for (const auto& label : request["labels"]) {
        if (!label.is_string()) {
            return systemone::invalid_decision_request("\"labels\" must be an array of strings");
        }
        labels.push_back(label.get<std::string>());
    }
    if (const std::string reason = validate_label_list(labels); !reason.empty()) {
        return systemone::invalid_decision_request(reason);
    }
    const int top_k = request.contains("top_k") && request["top_k"].is_number_integer()
                          ? request["top_k"].get<int>()
                          : 0;

    if (encoder_max_input_tokens_ <= 0) {
        return ErrorResponse::create("the prompt-router encoder has no token window",
                                     ErrorType::BACKEND_ERROR);
    }
    const size_t window = static_cast<size_t>(encoder_max_input_tokens_);

    // Carries llama-server's own error response out of the tokenizer callback.
    struct TokenizeFailure {
        json response;
    };
    auto tokens_of = [&transport](const std::string& content, bool add_special) {
        json body = {{"content", content}, {"add_special", add_special}};
        if (!add_special) body["with_pieces"] = true;
        json response = transport.post("/tokenize", body);
        if (response.contains("error")) throw TokenizeFailure{std::move(response)};
        return response.value("tokens", json());
    };

    try {
        size_t n_specials = 0;
        size_t elision_tokens = 0;
        {
            std::lock_guard<std::mutex> lock(rule_head_counts_mutex_);
            if (!rule_head_counts_) {
                rule_head_counts_.emplace(tokens_of("", true).size(),
                                          tokens_of("\n...\n", false).size());
            }
            std::tie(n_specials, elision_tokens) = *rule_head_counts_;
        }

        const RouterFit fit = fit_router_input(
            labels, text.get<std::string>(), window, n_specials, elision_tokens,
            [&](const std::string& content) { return tokens_of(content, false); });
        bool truncated = fit.truncated;

        RouterTokens tokens = router_tokens(
            tokens_of(fit.input.text, true), fit.tokens, fit.input.text.size());
        if (tokens.ids.size() > window) {
            tokens.ids.resize(window);
            tokens.spans.resize(window);
            truncated = true;
        }
        const RouterTokenGroups groups =
            group_router_tokens(fit.input, tokens.spans, labels);

        json embedded = transport.post("/embedding", json{{"content", tokens.ids}});
        if (embedded.is_object() && embedded.contains("error")) {
            return encoder_max_input_tokens_ > 0
                       ? map_encoder_overflow_error(std::move(embedded),
                                                              encoder_max_input_tokens_)
                       : embedded;
        }
        if (!embedded.is_array() || embedded.empty() || !embedded[0].is_object() ||
            !embedded[0].contains("embedding")) {
            throw std::runtime_error("llama-server /embedding returned no token vectors");
        }
        const std::vector<double> probabilities = score_router_labels(
            *rule_head_, embedded[0]["embedding"], tokens.ids.size(), groups);
        return router_classify_response(labels, probabilities, top_k, truncated);
    } catch (const TokenizeFailure& failure) {
        return failure.response;
    } catch (const std::invalid_argument& e) {
        return systemone::invalid_decision_request(e.what());
    } catch (const std::exception& e) {
        return ErrorResponse::create(e.what(), ErrorType::BACKEND_ERROR);
    }
}

std::optional<json> DecisionRuntime::laya_cut_error(const DecisionTransport& transport, const nlohmann::ordered_json& body) {
    if (decision_type_ != "laya" || decision_max_head_tokens_ <= 0) {
        return std::nullopt;
    }
    const std::string uncounted =
        "could not count the tokens of the question, so lemond cannot check that the model "
        "reads it whole";
    auto count_tokens = [&transport, &uncounted](const std::string& content) {
        const json counted = transport.post("/tokenize", json{{"content", content}});
        if (!counted.contains("tokens") || !counted["tokens"].is_array()) {
            throw std::runtime_error(uncounted);
        }
        return counted["tokens"].size();
    };
    const std::vector<LayaQuestionText> questions = laya_question_texts(body);
    for (const auto& question : questions) {
        std::vector<size_t> option_bytes;
        for (const auto& option : question.options) option_bytes.push_back(1 + option.size());
        // A byte-level BPE never has more tokens than bytes, so a short question
        // needs no /tokenize round trip.
        if (laya_cut(question, option_bytes, question.question.size(),
                               decision_max_head_tokens_).empty()) {
            continue;
        }
        std::vector<size_t> option_tokens;
        size_t question_tokens = 0;
        try {
            for (const auto& option : question.options) {
                option_tokens.push_back(count_tokens(" " + option));
            }
            question_tokens = count_tokens(question.question);
        } catch (const std::exception& e) {
            const std::string reason = e.what();
            return ErrorResponse::create(reason == uncounted ? reason : uncounted + ": " + reason,
                                         ErrorType::BACKEND_ERROR);
        }
        const std::string cut = laya_cut(question, option_tokens, question_tokens,
                                                   decision_max_head_tokens_);
        if (!cut.empty()) {
            return systemone::invalid_decision_request(
                questions.size() > 1 ? "question '" + question.id + "': " + cut : cut);
        }
    }
    return std::nullopt;
}

nlohmann::ordered_json DecisionRuntime::decide(const DecisionTransport& transport, const nlohmann::ordered_json& body) {
    // ort-server refuses a label it would cut; laya cuts options and the question
    // silently, so refuse them here.
    if (const auto error = laya_cut_error(transport, body)) {
        return nlohmann::ordered_json(*error);
    }
    nlohmann::ordered_json response = forward_systemone(transport, body);
    if (!response.contains("error")) {
        mark_answers_truncated(response, decision_type_, body);
    }
    return response;
}

nlohmann::ordered_json DecisionRuntime::systemone(const DecisionTransport& transport, const nlohmann::ordered_json& request) {
    if (!decision_model_) {
        return unsupported_decision_request("SystemOne");
    }
    // As normalize_response_model, without re-sorting the answer.
    nlohmann::ordered_json response = decide(transport, request);
    if (response.is_object() && response.contains("model")) {
        response["model"] = request.value("model", model_name_);
    }
    return response;
}

nlohmann::ordered_json DecisionRuntime::forward_systemone(const DecisionTransport& transport, const nlohmann::ordered_json& body) {
    nlohmann::ordered_json response = transport.post_ordered("/v1/systemone", body);
    if (encoder_max_input_tokens_ > 0 && response.contains("error")) {
        response = map_encoder_overflow_error(json(response), encoder_max_input_tokens_);
    }
    return response;
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
