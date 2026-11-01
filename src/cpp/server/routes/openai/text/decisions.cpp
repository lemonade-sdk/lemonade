#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/model_types.h"
#include "lemon/models/model_info.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNotDecisionModel{
    400, "",
    "`model` lacks the `classification` or `systemone` label, or its backend does not answer "
    "SystemOne questions; checked before it loads"};
const RouteError kNotTranslatable{
    400, "",
    "`input` or a question has something `/v1/systemone` cannot carry, such as an image URL, a "
    "developer message or an image in `instructions`, or a `laya` model would cut an option"};
const RouteError kContextExceeded{
    400, "context_length_exceeded", "`input` and a question do not fit in the model's context"};
const RouteError kNoImageInput{
    501, "", "`input` has an image for a model that reads no images, or that loaded without `mmproj`"};

const RouteArg kInput{"input", ArgIn::JsonBody, {{"type", json::array({"string", "array"})}}, true,
                      Support::Partial,
                      "The content to judge: a string, or an array of `user` messages whose `content` is "
                      "a string or a list of `input_text`, `input_image` and `input_json` parts. Images "
                      "must be data URLs. Other roles and tool items are refused."};
const RouteArg kQuestions{"questions", ArgIn::JsonBody, {{"type", "array"}}, true, Support::Available,
                          "1 to 400 questions, answered in this order. Each has a `type` (`predicate`, "
                          "`choice` or `score`), `instructions`, and an optional `name` that its answer "
                          "repeats. A `choice` has 2 to 255 `choices` (`{value, description}`, where "
                          "`value` is a string or a boolean); a `score` has 2 to 10 `levels` "
                          "(`{label, description}`), lowest first."};
const RouteArg kPromptCacheKey{"prompt_cache_key", ArgIn::JsonBody, {{"type", "string"}}, false,
                               Support::NotAvailable, "Accepted for OpenAI compatibility and ignored."};
const RouteArg kPromptCacheOptions{"prompt_cache_options", ArgIn::JsonBody, {{"type", "object"}}, false,
                                   Support::NotAvailable,
                                   "Accepted for OpenAI compatibility and ignored."};
const RouteArg kSafetyIdentifier{"safety_identifier", ArgIn::JsonBody, {{"type", "string"}}, false,
                                 Support::NotAvailable, "Accepted for OpenAI compatibility and ignored."};

class DecisionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.decisions";
        s.methods = {"POST"};
        s.paths = {"decisions"};
        s.summary = "Answer OpenAI Decisions questions with a decision model";
        s.modality = "input + questions -> one answer per question";
        s.experimental = true;
        s.description =
            "Implements the OpenAI Decisions API with a llama.cpp decision model. lemond turns the "
            "request into one `/v1/systemone` request, keeping the order of the questions and of "
            "each question's options, and turns the answers back. Only `llamacpp` models labelled "
            "`classification` and `systemone`, such as `Laya-GGUF`, serve this endpoint.";
        s.notes = {
            "### Translation\n\n"
            "A `predicate` is asked as a `noul` question. A `choice` option is keyed by its "
            "`value`, so a boolean is the text `true` or `false`, and a string with the same text "
            "is refused. A `score` level with a `description` reads `label: description`. The "
            "answers never have the type `refusal`. `confidence` is the decision model's own, not "
            "OpenAI's, and the cache counts in `usage` are always `0`.",
        };
        s.args = {model_loading::kModel, kInput, kQuestions, kPromptCacheKey, kPromptCacheOptions,
                  kSafetyIdentifier, model_loading::kCtxSize};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "answers", "usage"],
            "properties": {
                "model": {"type": "string", "description": "The model that answered."},
                "answers": {
                    "type": "array",
                    "description": "One answer for each question, in question order.",
                    "items": {
                        "type": "object",
                        "description": "One question's answer. Its fields depend on its `type`.",
                        "required": ["type", "name"],
                        "properties": {
                            "type": {"enum": ["predicate", "choice", "score"], "description": "The question's type."},
                            "name": {"type": ["string", "null"], "description": "The question's `name`, or `null` when it has none."},
                            "probability": {"type": "number", "description": "`predicate` answers: the probability that the statement is true."},
                            "choice": {"type": ["string", "boolean"], "description": "`choice` answers: the most likely `value`."},
                            "score": {"type": "number", "description": "`score` answers: the expected level index, from `0` to the number of levels minus one."},
                            "probabilities": {
                                "type": "array",
                                "description": "`choice` and `score` answers: one entry per option or level, in request order.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "value": {"type": ["string", "boolean", "integer"], "description": "The choice `value`, or the level index."},
                                        "label": {"type": "string", "description": "`score` answers: the level's `label`."},
                                        "probability": {"type": "number", "description": "The probability, in `[0, 1]`."}
                                    }
                                }
                            },
                            "confidence": {"type": "number", "description": "`choice` and `score` answers: how clearly the model prefers its answer."}
                        }
                    }
                },
                "usage": {
                    "type": "object",
                    "description": "Tokens processed, in OpenAI's shape.",
                    "properties": {
                        "input_tokens": {"type": "integer", "description": "Prompt tokens across all questions."},
                        "input_tokens_details": {"type": "object", "description": "`cached_tokens` and `cache_write_tokens`, always `0`."},
                        "output_tokens": {"type": "integer", "description": "Always `0`, because the model generates no tokens."},
                        "output_tokens_details": {"type": "object", "description": "`reasoning_tokens`, always `0`."},
                        "total_tokens": {"type": "integer", "description": "The same as `input_tokens`."}
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Laya-GGUF",
            "input": "Customer message: I was charged twice for my order last week and nobody has replied.",
            "questions": [
                {"type": "predicate", "name": "angry", "instructions": "Is the customer angry?"},
                {"type": "choice", "name": "team", "instructions": "Which team should handle this?",
                 "choices": [{"value": "billing"}, {"value": "shipping"}, {"value": "technical"}]},
                {"type": "score", "name": "urgency", "instructions": "How urgent is this?",
                 "levels": [{"label": "can wait"}, {"label": "this week"}, {"label": "today"},
                            {"label": "right now"}]}
            ]
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNotDecisionModel,
                                 kNotTranslatable, kContextExceeded, kNoImageInput, kModelUnloaded,
                                 kBackendError, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        if (const auto info = ctx_.router->try_get_model_info(req.model)) {
            if (!has_label(info->labels, "systemone") || info->type != ModelType::CLASSIFICATION ||
                !backends::recipe_gates_mode_on(info->recipe, "systemone")) {
                write_openai_error(res, kNotDecisionModel,
                    "model '" + req.model + "' is not a SystemOne decision model (it needs "
                    "the 'classification' and 'systemone' labels on a backend that answers "
                    "/v1/systemone, such as llamacpp)");
                return false;
            }
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        ordered_json body = ordered_json::parse(req.http.body);
        body[model_loading::kModel.name] = req.model;
        const ordered_json response = ctx_.router->decisions(body);
        if (response.contains("error")) {
            set_error_response(json(response), res, kBackendError);
            return;
        }
        res.set_content(response.dump(), "application/json");
    }

    LoadSpan load_span() const override { return {"CLASSIFIER", "decisions"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_decisions_route(ServerContext& ctx) {
    return std::make_unique<DecisionsRoute>(ctx);
}

} // namespace lemon
