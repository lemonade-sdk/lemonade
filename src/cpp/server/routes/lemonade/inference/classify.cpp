#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidClassifyBody{400, "", "The body is empty or is not valid JSON"};
const RouteError kNonStringModel{400, "", "`model` is not a string"};
const RouteError kMissingInput{400, "", "Neither `input` nor `text` is present"};
const RouteError kNonStringInput{400, "", "`input` or `text` is not a string"};
const RouteError kBlankInput{400, "", "The input text is empty or only whitespace"};
const RouteError kInvalidTopK{400, "", "`top_k` is not an integer from 1 to 1,000,000"};
const RouteError kNoSoleClassifier{
    400, "", "`model` is omitted and not exactly one classification model is loaded"};
const RouteError kBackendRejected{
    400, "",
    "The backend rejects the request, for example because `model` is not a classification "
    "model, which is found only after it loads"};
const RouteError kModelNotLoaded{
    404, "", "The model is unloaded between its load and the classification"};
const RouteError kUnexpectedResponse{500, "", "The backend's response has no `labels` object"};

class ClassifyRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.classify";
        s.methods = {"POST"};
        s.paths = {"classify"};
        s.summary = "Score text against a classifier model's labels";
        s.experimental = true;
        s.description =
            "Runs an encoder text classifier, such as a PII, prompt-safety or domain classifier, on "
            "a string and returns a score for each label. It serves `onnxruntime` models, both "
            "sequence classifiers (one label set) and token classifiers (aggregated span labels), "
            "and provides the classification that the router's `classifier` condition uses.";
        s.notes = {
            "### Supported Models\n\n"
            "Single-sequence encoder families: BERT, DistilBERT, RoBERTa, XLM-RoBERTa, DeBERTa "
            "(v1/v2), ELECTRA, ALBERT and CamemBERT. A stock `optimum-cli export onnx` directory of "
            "one of these works as is.\n\n"
            "A servable model directory holds `model.onnx`, `tokenizer.json` and `config.json`. The "
            "`config.json` is always required: its architecture is checked against the families "
            "above, so an unsupported family such as XLNet is rejected at load time rather than "
            "served with wrong scores. Label names (`id2label`), normalization and the token budget "
            "come from the same config, and an optional `manifest.json` overrides them. Without a "
            "manifest, inference assumes single-label softmax, so a multi-label (sigmoid) model "
            "declares `problem_type: multi_label_classification` in its config or ships a "
            "`manifest.json`.",
            "Some upstream models declare only generic `LABEL_<n>` label names; their model card "
            "explains them.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "The classifier model. Optional when exactly one classification model is loaded; that "
             "model is used."},
            {"input", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Text to classify. Required unless `text` is given."},
            {"top_k", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}}, false, Support::Available,
             "Return only the highest-scoring `k` labels. Defaults to every label."},
            {"text", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Alias for `input`, used instead of it when both are given."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["object", "model", "labels"],
            "properties": {
                "object": {"const": "classification", "description": "Always `classification`."},
                "model": {"type": "string", "description": "The model that classified the input, including one chosen because it was the only one loaded."},
                "labels": {
                    "type": "object",
                    "description": "Each label's score, keyed by label name.",
                    "additionalProperties": {"type": "number", "description": "The label's score, in `[0, 1]`."}
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Phishing-Email-Detection-ONNX",
            "input": "Please verify your account at http://secure-login.example now."
        })");
        s.examples = {example};
        s.errors = {kNonStringModel, kMissingInput, kNonStringInput, kBlankInput, kInvalidTopK,
                    kNoSoleClassifier, kBackendRejected, kModelNotLoaded, kBackendError,
                    kUnexpectedResponse};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;

        // Malformed requests are rejected before any model gets loaded.
        std::string validation_error;
        const RouteError* validation_route_error = nullptr;
        bool has_text = request_json.contains("text");
        bool has_input = request_json.contains("input");
        const auto& input_field = has_text ? request_json["text"]
                                 : has_input ? request_json["input"] : request_json;
        if (request_json.contains("model") && !request_json["model"].is_string()) {
            validation_error = "'model' must be a string";
            validation_route_error = &kNonStringModel;
        } else if (!has_text && !has_input) {
            validation_error = "Missing 'input' (or 'text') string in classify request";
            validation_route_error = &kMissingInput;
        } else if (has_text && !request_json["text"].is_string()) {
            validation_error = "'text' must be a string";
            validation_route_error = &kNonStringInput;
        } else if (has_input && !request_json["input"].is_string()) {
            validation_error = "'input' must be a string";
            validation_route_error = &kNonStringInput;
        } else if (input_field.get<std::string>().find_first_not_of(" \t\r\n") ==
                   std::string::npos) {
            validation_error = "input text must not be empty";
            validation_route_error = &kBlankInput;
        } else if (request_json.contains("top_k") &&
                   (!request_json["top_k"].is_number_integer() ||
                    request_json["top_k"].get<long long>() < 1 ||
                    request_json["top_k"].get<long long>() > 1000000)) {
            validation_error = "'top_k' must be a positive integer";
            validation_route_error = &kInvalidTopK;
        }
        if (validation_route_error) {
            write_openai_error(res, *validation_route_error, validation_error);
            return false;
        }

        // The router requires a model on every request, so an omitted one is resolved
        // here, which is only possible when exactly one classifier is loaded.
        if (!req.body.contains("model")) {
            req.model = ctx_.router->get_sole_loaded_model_of_type(ModelType::CLASSIFICATION);
            if (req.model.empty()) {
                write_openai_error(res, kNoSoleClassifier,
                    "No 'model' specified and no single classification model "
                    "is loaded (load one, or name it in the request)");
                return false;
            }
            req.body["model"] = req.model;
            req.model_already_loaded = true;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->classify(req.body);
        if (response.contains("error") && response["error"].is_object()) {
            const auto& err = response["error"];
            if (err.contains("status_code") && err["status_code"].is_number_integer()) {
                res.status = err["status_code"].get<int>();
            } else if (err.contains("code") && err["code"].is_string()) {
                res.status = ModelLoader::load_error(err["code"].get<std::string>()).status;
            } else if (err.contains("type") && err["type"].is_string()) {
                // LemonException::to_json carries only {message, type}.
                const std::string type = err["type"].get<std::string>();
                if (type == "invalid_request" || type == "invalid_request_error" ||
                    type == "unsupported_operation") {
                    res.status = kBackendRejected.status;
                } else if (type == "model_not_loaded") {
                    res.status = kModelNotLoaded.status;
                } else {
                    res.status = kBackendError.status;
                }
            } else {
                res.status = kBackendError.status;
            }
            res.set_content(response.dump(), "application/json");
            return;
        }

        // The envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("labels") || !response["labels"].is_object()) {
            write_openai_error(res, kUnexpectedResponse,
                               "Classification backend returned an unexpected response",
                               "classification_error");
            return;
        }
        json enveloped = {
            {"object", "classification"},
            {"model", req.model.empty() ? ctx_.router->get_loaded_model() : req.model},
            {"labels", response["labels"]},
        };
        res.set_content(enveloped.dump(), "application/json");
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_openai_error(res, kInvalidClassifyBody,
                           std::string("Invalid JSON in request body: ") + error.what());
    }
    std::vector<RouteError> invalid_body_errors() const override { return {kInvalidClassifyBody}; }

    LoadSpan load_span() const override { return {"CLASSIFIER", "classify"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_classify_route(ServerContext& ctx) {
    return std::make_unique<ClassifyRoute>(ctx);
}

} // namespace lemon
