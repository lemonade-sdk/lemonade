#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class ClassifyRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.classify";
        s.methods = {"POST"};
        s.paths = {"classify"};
        s.summary = "Classify input text with an encoder classifier (label scores)";
        s.experimental = true;
        s.description =
            "Runs an encoder text classifier, such as a PII, prompt-safety or domain classifier, "
            "on a string and returns a score in `[0, 1]` for each label.";
        s.notes = {
            "The model must use the `onnxruntime` recipe; see "
            "[Classifier Models](#classifier-models) for the architectures it serves. Both "
            "sequence-classification models (one label set) and token-classification models "
            "(aggregated span labels) are supported.",
            "Label names come from the model's `id2label`, in `config.json` or in a "
            "`manifest.json` that overrides it. Some upstream models only declare generic "
            "`LABEL_<n>` names; see the model card for their meaning.",
            "A malformed request (invalid JSON, a missing `input`, a non-string field, or a "
            "`top_k` that is not a positive integer) is answered with `400` before any model "
            "is loaded.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Classifier model, using the `onnxruntime` recipe; loaded on first use. Optional "
             "when exactly one classification model is loaded, which then serves the request."},
            {"input", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text to classify."},
            {"text", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Alias for `input`."},
            {"top_k", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}}, false, Support::Available,
             "Return only the `top_k` highest-scoring labels."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["object", "model", "labels"],
            "properties": {
                "object": {"const": "classification"},
                "model": {"type": "string", "description": "The model that classified the input, including one chosen because it was the only one loaded."},
                "labels": {
                    "type": "object",
                    "description": "Score in [0, 1] for each label.",
                    "additionalProperties": {"type": "number"}
                }
            }
        })");
        response.example = json::parse(R"({
            "model": "Phishing-Email-Detection-ONNX",
            "input": "Please verify your account at http://secure-login.example now."
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;

        // Malformed requests are rejected before any model gets loaded.
        std::string validation_error;
        bool has_text = request_json.contains("text");
        bool has_input = request_json.contains("input");
        const auto& input_field = has_text ? request_json["text"]
                                 : has_input ? request_json["input"] : request_json;
        if (request_json.contains("model") && !request_json["model"].is_string()) {
            validation_error = "'model' must be a string";
        } else if (!has_text && !has_input) {
            validation_error = "Missing 'input' (or 'text') string in classify request";
        } else if (has_text && !request_json["text"].is_string()) {
            validation_error = "'text' must be a string";
        } else if (has_input && !request_json["input"].is_string()) {
            validation_error = "'input' must be a string";
        } else if (input_field.get<std::string>().find_first_not_of(" \t\r\n") ==
                   std::string::npos) {
            validation_error = "input text must not be empty";
        } else if (request_json.contains("top_k") &&
                   (!request_json["top_k"].is_number_integer() ||
                    request_json["top_k"].get<long long>() < 1 ||
                    request_json["top_k"].get<long long>() > 1000000)) {
            validation_error = "'top_k' must be a positive integer";
        }
        if (!validation_error.empty()) {
            write_openai_error(res, 400, validation_error);
            return false;
        }

        // The router requires a model on every request, so an omitted one is resolved
        // here, which is only possible when exactly one classifier is loaded.
        if (!req.body.contains("model")) {
            req.model = ctx_.router->get_sole_loaded_model_of_type(ModelType::CLASSIFICATION);
            if (req.model.empty()) {
                write_openai_error(res, 400,
                    "No 'model' specified and no single classification model "
                    "is loaded (load one, or name it in the request)");
                return false;
            }
            req.body["model"] = req.model;
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
                res.status = ModelLoader::load_error_status(err["code"].get<std::string>());
            } else if (err.contains("type") && err["type"].is_string()) {
                // LemonException::to_json carries only {message, type}.
                const std::string type = err["type"].get<std::string>();
                if (type == "invalid_request" || type == "invalid_request_error" ||
                    type == "unsupported_operation") {
                    res.status = 400;
                } else if (type == "model_not_loaded") {
                    res.status = 404;
                } else {
                    res.status = 500;
                }
            } else {
                res.status = 500;
            }
            res.set_content(response.dump(), "application/json");
            return;
        }

        // The envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("labels") || !response["labels"].is_object()) {
            write_openai_error(res, 500, "Classification backend returned an unexpected response",
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
        write_openai_error(res, 400, std::string("Invalid JSON in request body: ") + error.what());
    }

    LoadSpan load_span() const override { return {"CLASSIFIER", "classify"}; }
};

} // namespace

std::unique_ptr<ApiRoute> make_classify_route(ServerContext& ctx) {
    return std::make_unique<ClassifyRoute>(ctx);
}

} // namespace lemon
