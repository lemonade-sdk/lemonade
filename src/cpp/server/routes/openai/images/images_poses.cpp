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

const RouteError kNoSolePoseModel{
    400, "", "`model` is omitted and not exactly one pose model is loaded"};
const RouteError kBackendRejected{
    400, "",
    "The backend rejects the request, for example because `model` is not a pose model, which "
    "is found only after it loads"};

const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                      "The pose model, such as `YOLO26s-Pose-NPU`. Optional when exactly one pose "
                      "model is loaded; that model is used."};
const RouteArg kConfidence{"confidence", ArgIn::JsonBody,
                           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, false,
                           Support::Available,
                           "Minimum person score a pose needs to be returned. Defaults to `0.25`. "
                           "Keypoints are returned whatever their own score."};
const RouteArg kMaxPoses{
    "max_poses", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}},
    false, Support::Available,
    "Maximum poses returned per image, highest score first. Defaults to `20`."};

class ImagesPosesRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_poses";
        s.methods = {"POST"};
        s.paths = {"images/poses"};
        s.summary = "Estimate human poses in images";
        s.modality = "image -> people with body keypoints";
        s.experimental = true;
        s.description =
            "Finds people in one or more images and returns, for each, a score, a bounding box and "
            "named body keypoints (the 17 COCO keypoints: nose, eyes, ears, shoulders, elbows, "
            "wrists, hips, knees and ankles). It follows the conventions of "
            "[`POST /v1/images/detections`](#post-v1imagesdetections) and serves "
            "`ryzenai-vision` pose models, which run on the Ryzen AI NPU.";
        s.notes = {
            "A malformed request, such as one whose `image` is not an image, is rejected before "
            "any model loads.",
            "### Setup\n\n"
            "Pose models use the `ryzenai-vision` backend; see "
            "[`POST /v1/images/detections`](#post-v1imagesdetections) for its one-time setup and "
            "the first-load NPU compile. A pose model and a detection model can be loaded on the "
            "NPU together.",
            "### Supported Models\n\n"
            "Ultralytics YOLO26-pose end-to-end exports (`[1, N, 6 + 3K]` output) and their "
            "Hugging Face exports such as `onnx-community/yolo26s-pose-ONNX`.",
        };
        s.args = {kModel, openai::vision::kImage, kConfidence, kMaxPoses};

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
                        "required": ["object", "index", "width", "height", "poses"],
                        "properties": {
                            "object": {"const": "image.poses", "description": "Always `image.poses`."},
                            "index": {"type": "integer", "description": "Position of the image in the request."},
                            "width": {"type": "integer", "description": "Width of the decoded image in pixels."},
                            "height": {"type": "integer", "description": "Height of the decoded image in pixels."},
                            "poses": {
                                "type": "array",
                                "description": "The people found, highest score first.",
                                "items": {
                                    "type": "object",
                                    "required": ["object", "label", "score", "box", "keypoints"],
                                    "properties": {
                                        "object": {"const": "pose", "description": "Always `pose`."},
                                        "label": {"type": "string", "description": "What was found, `person` for COCO pose models."},
                                        "score": {"type": "number", "description": "Confidence that this is a person, in `[0, 1]`."},
                                        "box": {
                                            "type": "object",
                                            "description": "Corners in pixels of the original image: `x1,y1` top-left, `x2,y2` bottom-right.",
                                            "required": ["x1", "y1", "x2", "y2"],
                                            "properties": {
                                                "x1": {"type": "number", "description": "Left edge."},
                                                "y1": {"type": "number", "description": "Top edge."},
                                                "x2": {"type": "number", "description": "Right edge."},
                                                "y2": {"type": "number", "description": "Bottom edge."}
                                            }
                                        },
                                        "keypoints": {
                                            "type": "array",
                                            "description": "Every keypoint of the model, in its order.",
                                            "items": {
                                                "type": "object",
                                                "required": ["name", "x", "y", "score"],
                                                "properties": {
                                                    "name": {"type": "string", "description": "Keypoint name, such as `left_wrist`."},
                                                    "x": {"type": "number", "description": "Horizontal position in pixels of the original image."},
                                                    "y": {"type": "number", "description": "Vertical position in pixels of the original image."},
                                                    "score": {"type": "number", "description": "Confidence that the keypoint is visible, in `[0, 1]`."}
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
                    "required": ["images", "poses", "inference_ms"],
                    "properties": {
                        "images": {"type": "integer", "description": "Images analyzed."},
                        "poses": {"type": "integer", "description": "Poses returned across all images."},
                        "inference_ms": {"type": "number", "description": "Time spent decoding, preprocessing and running the model for all images."}
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "YOLO26s-Pose-NPU",
            "image": "data:image/png;base64,@fixtures/image.png"
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kVisionImageInvalid,
                                 kNoSolePoseModel, kBackendRejected, kModelUnloaded, kBackendError,
                                 kMalformedBackendResponse, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/poses" << std::endl;

        if (!normalize_vision_images(req, res)) return false;
        if (req.has(kMaxPoses)) {
            // The validator accepts 2.0 as an integer; the backend expects 2.
            req.body[kMaxPoses.name] = static_cast<long long>(req.arg(kMaxPoses).get<double>());
        }
        if (!req.has(kModel)) {
            if (!resolve_sole_vision_model(ctx_, req, res, ModelType::POSE, kNoSolePoseModel)) return false;
            req.body[kModel.name] = req.model;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        const size_t image_count = req.arg(openai::vision::kImage).size();
        auto response = ctx_.router->image_poses(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }

        // The envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("data") || !response["data"].is_array() ||
            response["data"].size() != image_count) {
            write_openai_error(res, kMalformedBackendResponse,
                               "Pose backend returned an unexpected response", "server_error");
            return;
        }
        json data = json::array();
        size_t total = 0;
        for (const auto& item : response["data"]) {
            json poses = json::array();
            for (const auto& pose : item.value("poses", json::array())) {
                poses.push_back({
                    {"object", "pose"},
                    {"label", pose.value("label", "person")},
                    {"score", pose.value("score", 0.0)},
                    {"box", pose.value("box", json::object())},
                    {"keypoints", pose.value("keypoints", json::array())},
                });
            }
            total += poses.size();
            data.push_back({
                {"object", "image.poses"},
                {"index", item.value("index", static_cast<int>(data.size()))},
                {"width", item.value("width", 0)},
                {"height", item.value("height", 0)},
                {"poses", std::move(poses)},
            });
        }
        json enveloped = {
            {"object", "list"},
            {"created", static_cast<long long>(std::time(nullptr))},
            {"model", req.model.empty() ? ctx_.router->get_loaded_model() : req.model},
            {"data", std::move(data)},
            {"usage", {
                {"images", image_count},
                {"poses", total},
                {"inference_ms", response.value("inference_ms", 0.0)},
            }},
        };
        res.set_content(enveloped.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_poses_route(ServerContext& ctx) {
    return std::make_unique<ImagesPosesRoute>(ctx);
}

} // namespace lemon
