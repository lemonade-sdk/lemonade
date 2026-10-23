#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using classification::kModel;

const RouteError kNoSoleClassifier{
    400, "", "`model` is omitted and not exactly one classification model is loaded"};
const RouteError kNotDecisionModel{
    400, "", "`model` lacks the `classification` or `systemone` label, checked before it loads"};
const RouteError kQuestionsRejected{
    400, "",
    "`llama-server` rejects a question, such as a `score` question with one level; the backend's "
    "status replaces this one"};
const RouteError kContextExceeded{
    400, "context_length_exceeded", "`state` and a question do not fit in the model's context"};
const RouteError kNoImageInput{
    501, "", "`images` is sent to a model that reads no images, or that loaded without `mmproj`"};

const RouteArg kState{"state", ArgIn::JsonBody, {{"type", json::array({"string", "object", "array"})}},
                      true, Support::Available,
                      "The content to judge. A value that is not a string reaches the model as JSON text."};
const RouteArg kQuestions{"questions", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
                          "Maps each question id to a question, which the model answers independently of the "
                          "others. A question has a `type` (`choice`, `score` or `noul`), `instructions`, and "
                          "`criteria`: for `choice`, an object that maps each option to a description or "
                          "`null`; for `score`, an array of 2 to 10 levels, lowest first; for `noul`, optional "
                          "`true` and `false` descriptions."};
const RouteArg kImages{"images", ArgIn::JsonBody, {{"type", "array"}, {"items", {{"type", "string"}}}}, false,
                       Support::Partial,
                       "Data URLs of images to judge with the state. Only decision models of the `openjev` "
                       "type that load with an `mmproj` file read them.",
                       {}, true};

class SystemOneRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "llamacpp.systemone";
        s.methods = {"POST"};
        s.paths = {"systemone"};
        s.summary = "Answer typed questions about content with a decision model";
        s.modality = "state + questions -> one answer per question";
        s.experimental = true;
        s.description =
            "Asks a llama.cpp decision model typed questions about one piece of content and returns "
            "one answer per question, without generating tokens. lemond forwards the request to "
            "`llama-server` unchanged, including the order of each question's options, and "
            "`llama-server` builds the prompt from the template in the GGUF. Only `llamacpp` models "
            "labelled `classification` and `systemone`, such as `Laya-GGUF`, serve this endpoint.";
        s.notes = {
            "### Answer Quality\n\n"
            "Answers depend on the model, on the wording of each question, and on the order of a "
            "`choice` question's options. Test a question with the model that serves it before a "
            "router policy relies on its scores.",
        };
        s.args = {kModel, kState, kQuestions, kImages, model_loading::kCtxSize};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model", "answers"],
            "properties": {
                "model": {"type": "string", "description": "The model that answered."},
                "answers": {
                    "type": "object",
                    "description": "One answer for each question, keyed by question id.",
                    "additionalProperties": {
                        "type": "object",
                        "description": "One question's answer. Its fields depend on the question's `type`.",
                        "required": ["type"],
                        "properties": {
                            "type": {"enum": ["choice", "score", "noul"], "description": "The question's type."},
                            "choice": {"type": "string", "description": "`choice` questions: the most likely option."},
                            "score": {"type": "number", "description": "`score` questions: the expected level index, from `0` to the number of levels minus one."},
                            "legend": {
                                "type": "object",
                                "description": "`score` questions: each level's text, keyed by level index.",
                                "additionalProperties": {"type": "string", "description": "The level's text."}
                            },
                            "probabilities": {
                                "type": "object",
                                "description": "`choice` and `score` questions: the probability of each option, keyed by option for `choice` and by level index for `score`. The probabilities sum to `1`.",
                                "additionalProperties": {"type": "number", "description": "The probability, in `[0, 1]`."}
                            },
                            "confidence": {"type": "number", "description": "`choice` and `score` questions: how clearly the model prefers its answer; `0` when every option is equally likely."},
                            "noul": {"type": "number", "description": "`noul` questions: the probability that the answer is `true`."}
                        }
                    }
                },
                "usage": {
                    "type": "object",
                    "description": "Tokens processed.",
                    "properties": {
                        "input_tokens": {"type": "integer", "description": "Prompt tokens across all questions."},
                        "output_tokens": {"type": "integer", "description": "Always `0`, because the model generates no tokens."}
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Laya-GGUF",
            "state": "Customer message: I was charged twice for my order last week and nobody has replied.",
            "questions": {
                "route": {"type": "choice", "instructions": "Which team should handle this?",
                          "criteria": {"billing": null, "shipping": null, "technical": null}},
                "angry": {"type": "noul", "instructions": "Is the customer angry?"},
                "urgency": {"type": "score", "instructions": "How urgent is this?",
                            "criteria": ["can wait", "this week", "today", "right now"]}
            }
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kNoSoleClassifier,
                                 kNotDecisionModel, kQuestionsRejected, kContextExceeded,
                                 kNoImageInput, kModelUnloaded, kBackendError,
                                 kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        if (req.arg(kQuestions).empty()) {
            write_openai_error(res, kArgInvalid, "\"questions\" must be a non-empty object");
            return false;
        }

        if (!req.has(kModel)) {
            req.model = ctx_.router->get_sole_loaded_model_of_type(ModelType::CLASSIFICATION);
            if (req.model.empty()) {
                write_openai_error(res, kNoSoleClassifier,
                    "No 'model' specified and no single classification model "
                    "is loaded (load one, or name it in the request)");
                return false;
            }
            req.body[kModel.name] = req.model;
            req.model_already_loaded = true;
        }

        // Refused before the load: a chat model would take an LLM slot, and evict
        // the user's chat model, only to fail.
        if (ctx_.model_manager->model_exists(req.model)) {
            const ModelInfo info = ctx_.model_manager->get_model_info(req.model);
            if (!has_label(info.labels, "systemone") || info.type != ModelType::CLASSIFICATION) {
                write_openai_error(res, kNotDecisionModel,
                    "model '" + req.model + "' is not a SystemOne decision model (it needs "
                    "the 'classification' and 'systemone' labels)");
                return false;
            }
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        // req.body sorts its keys, and the order of a choice question's options
        // changes the model's scores.
        ordered_json body = ordered_json::parse(req.http.body);
        body[kModel.name] = req.model;
        const ordered_json response = ctx_.router->systemone(body);
        if (response.contains("error")) {
            set_error_response(json(response), res, kBackendError);
            return;
        }
        res.set_content(response.dump(), "application/json");
    }

    LoadSpan load_span() const override { return {"CLASSIFIER", "systemone"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_systemone_route(ServerContext& ctx) {
    return std::make_unique<SystemOneRoute>(ctx);
}

} // namespace lemon
