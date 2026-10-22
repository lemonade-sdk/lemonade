#include <ctime>
#include <string>
#include <utility>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"
#include "lemon/server/vision_inputs.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNoSoleHandsModel{
    400, "", "`model` is omitted and not exactly one hand-tracking model is loaded"};
const RouteError kBackendRejected{
    400, "",
    "The backend rejects the request, for example because `model` is not a hand-tracking "
    "model, which is found only after it loads"};

const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                      "The hand-tracking model, such as `MediaPipe-Hands-CPU`. Optional when exactly "
                      "one hand-tracking model is loaded; that model is used."};
const RouteArg kConfidence{"confidence", ArgIn::JsonBody,
                           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, false,
                           Support::Available,
                           "Minimum hand detection and presence score. Defaults to `0.5`."};
const RouteArg kMaxHands{
    "max_hands", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}},
    false, Support::Available, "Maximum hands returned per image. Defaults to `2`."};
const RouteArg kMirrored{
    "mirrored", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
    "`true` when the images are mirrored, as a selfie camera preview is. `handedness` is the "
    "person's own hand either way. Defaults to `false`."};

class ImagesHandsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_hands";
        s.methods = {"POST"};
        s.paths = {"images/hands"};
        s.summary = "Track hands in images";
        s.modality = "image -> hands with landmarks";
        s.experimental = true;
        s.description =
            "Finds hands in one or more images and returns, for each, its handedness, a score, a "
            "bounding box and the 21 MediaPipe hand landmarks (wrist and four joints per finger). "
            "It follows the conventions of [`POST /v1/images/detections`](#post-v1imagesdetections) "
            "and serves `mediapipe` models, which run on the CPU.";
        s.notes = {
            "A malformed request, such as one whose `image` is not an image, is rejected before "
            "any model loads.",
            "### Setup\n\n"
            "The `mediapipe` backend runs in a Python venv that Lemonade builds with the pinned "
            "`mediapipe` wheel the first time a model loads, or with `lemonade backends install "
            "mediapipe:cpu`; it needs Python 3.9 or newer with venv support on `PATH`. To use an "
            "existing environment that has `mediapipe` instead, run `lemonade config set "
            "mediapipe.cpu_bin=<env dir or its python>`.",
        };
        s.args = {kModel, openai::vision::kImage, kConfidence, kMaxHands, kMirrored};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["object", "created", "model", "data", "usage"],
            "properties": {
                "object": {"const": "list", "description": "Always `list`."},
                "created": {"type": "integer", "description": "Unix timestamp of the response."},
                "model": {"type": "string", "description": "The model that ran, including one chosen because it was the only one loaded."},
                "data": {
                    "type": "array",
                    "description": "One entry per input image, in request order.",
                    "items": {
                        "type": "object",
                        "required": ["object", "index", "width", "height", "hands"],
                        "properties": {
                            "object": {"const": "image.hands", "description": "Always `image.hands`."},
                            "index": {"type": "integer", "description": "Position of the image in the request."},
                            "width": {"type": "integer", "description": "Width of the decoded image in pixels."},
                            "height": {"type": "integer", "description": "Height of the decoded image in pixels."},
                            "hands": {
                                "type": "array",
                                "description": "The hands found, highest score first.",
                                "items": {
                                    "type": "object",
                                    "required": ["object", "handedness", "score", "box", "landmarks"],
                                    "properties": {
                                        "object": {"const": "hand", "description": "Always `hand`."},
                                        "handedness": {"enum": ["left", "right"], "description": "Which of the person's hands it is."},
                                        "score": {"type": "number", "description": "Handedness confidence, in `[0, 1]`."},
                                        "box": {
                                            "type": "object",
                                            "description": "Bounds of the landmarks in pixels of the original image: `x1,y1` top-left, `x2,y2` bottom-right.",
                                            "required": ["x1", "y1", "x2", "y2"],
                                            "properties": {
                                                "x1": {"type": "number", "description": "Left edge."},
                                                "y1": {"type": "number", "description": "Top edge."},
                                                "x2": {"type": "number", "description": "Right edge."},
                                                "y2": {"type": "number", "description": "Bottom edge."}
                                            }
                                        },
                                        "landmarks": {
                                            "type": "array",
                                            "description": "The 21 landmarks, wrist first.",
                                            "items": {
                                                "type": "object",
                                                "required": ["name", "x", "y", "z"],
                                                "properties": {
                                                    "name": {"type": "string", "description": "Landmark name, such as `index_finger_tip`."},
                                                    "x": {"type": "number", "description": "Horizontal position in pixels of the original image."},
                                                    "y": {"type": "number", "description": "Vertical position in pixels of the original image."},
                                                    "z": {"type": "number", "description": "Depth relative to the wrist, in the same pixel scale as `x`; smaller is closer to the camera."}
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                },
                "usage": {
                    "type": "object",
                    "description": "What the request processed.",
                    "required": ["images", "hands", "inference_ms"],
                    "properties": {
                        "images": {"type": "integer", "description": "Images analyzed."},
                        "hands": {"type": "integer", "description": "Hands returned across all images."},
                        "inference_ms": {"type": "number", "description": "Time spent decoding and running the model for all images."}
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "MediaPipe-Hands-CPU",
            "image": "data:image/jpeg;base64,@fixtures/hand.jpg"
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kVisionImageInvalid,
                                 kNoSoleHandsModel, kBackendRejected, kModelUnloaded, kBackendError,
                                 kMalformedBackendResponse, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/hands" << std::endl;

        if (!normalize_vision_images(req, res)) return false;
        if (req.has(kMaxHands)) {
            // The validator accepts 2.0 as an integer; the backend expects 2.
            req.body[kMaxHands.name] = static_cast<long long>(req.arg(kMaxHands).get<double>());
        }
        if (!req.has(kModel)) {
            if (!resolve_sole_vision_model(ctx_, req, res, ModelType::HANDS, kNoSoleHandsModel)) return false;
            req.body[kModel.name] = req.model;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        const size_t image_count = req.arg(openai::vision::kImage).size();
        auto response = ctx_.router->image_hands(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }

        // The envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("data") || !response["data"].is_array() ||
            response["data"].size() != image_count) {
            write_openai_error(res, kMalformedBackendResponse,
                               "Hand-tracking backend returned an unexpected response", "server_error");
            return;
        }
        json data = json::array();
        size_t total = 0;
        for (const auto& item : response["data"]) {
            json hands = json::array();
            for (const auto& hand : item.value("hands", json::array())) {
                hands.push_back({
                    {"object", "hand"},
                    {"handedness", hand.value("handedness", "")},
                    {"score", hand.value("score", 0.0)},
                    {"box", hand.value("box", json::object())},
                    {"landmarks", hand.value("landmarks", json::array())},
                });
            }
            total += hands.size();
            data.push_back({
                {"object", "image.hands"},
                {"index", item.value("index", static_cast<int>(data.size()))},
                {"width", item.value("width", 0)},
                {"height", item.value("height", 0)},
                {"hands", std::move(hands)},
            });
        }
        json enveloped = {
            {"object", "list"},
            {"created", static_cast<long long>(std::time(nullptr))},
            {"model", req.model.empty() ? ctx_.router->get_loaded_model() : req.model},
            {"data", std::move(data)},
            {"usage", {
                {"images", image_count},
                {"hands", total},
                {"inference_ms", response.value("inference_ms", 0.0)},
            }},
        };
        res.set_content(enveloped.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_hands_route(ServerContext& ctx) {
    return std::make_unique<ImagesHandsRoute>(ctx);
}

} // namespace lemon
