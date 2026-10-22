#include <ctime>
#include <string>
#include <utility>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

constexpr size_t kMaxImages = 16;

const RouteError kNoSoleDetector{
    400, "", "`model` is omitted and not exactly one detection model is loaded"};
const RouteError kImageInvalid{
    400, "", "An `image` is not a PNG, JPEG, BMP or WebP image, is a remote URL, or there are more "
             "than 16"};
const RouteError kBackendRejected{
    400, "",
    "The backend rejects the request, for example because `model` is not a detection model, "
    "which is found only after it loads"};

const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                      "The detection model, such as `YOLO26s-Detection-NPU`. Optional when exactly one "
                      "detection model is loaded; that model is used."};
const RouteArg kImage{
    "image", ArgIn::JsonBody, {{"type", json::array({"string", "object", "array"})}}, true, Support::Available,
    "The image or images to analyze, PNG, JPEG, BMP or WebP. Each is a base64 string, a "
    "`data:image/...;base64,` URL, or an `{\"image_url\": \"...\"}` / "
    "`{\"image_url\": {\"url\": \"...\"}}` object as in OpenAI's image inputs; send an array of "
    "up to 16 for a batch. Remote `http(s)` URLs are rejected rather than fetched.",
    {"images"}};
const RouteArg kConfidence{"confidence", ArgIn::JsonBody,
                           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}, false,
                           Support::Available,
                           "Minimum score a detection needs to be returned. Defaults to `0.25`."};
const RouteArg kClasses{
    "classes", ArgIn::JsonBody,
    {{"type", json::array({"string", "array"})}, {"pattern", "\\S"},
     {"items", {{"type", "string"}, {"pattern", "\\S"}}}},
    false, Support::Available,
    "Return only these classes. Takes the model's class names (`sports ball`) and everyday names "
    "that map onto them (`ball`, `phone`, `mug`); a phrase such as `the red ball` works too. "
    "Names that match no class are listed in `unresolved_classes`."};
const RouteArg kMaxDetections{
    "max_detections", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}},
    false, Support::Available,
    "Maximum detections returned per image, highest score first. Defaults to `100`."};

bool is_detectable_image(const std::string& head) {
    const auto sniffed = utils::sniff_image(head);
    // GIF is left out: OpenCV older than 4.11 cannot decode it.
    if (sniffed.ok()) return sniffed.mime != "image/gif";
    return head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0;
}

// Returns the bare base64 payload of one image input, or "" with `error` set.
std::string image_payload(const json& item, std::string& error) {
    std::string value;
    if (item.is_string()) {
        value = item.get<std::string>();
    } else if (item.is_object() && item.contains("image_url")) {
        const json& url = item["image_url"];
        if (url.is_string()) {
            value = url.get<std::string>();
        } else if (url.is_object() && url.contains("url") && url["url"].is_string()) {
            value = url["url"].get<std::string>();
        }
    }
    if (value.empty()) {
        error = "must be a base64 string, a data URL, or {\"image_url\": ...}";
        return "";
    }
    // Never fetched, so the server makes no outbound request on a client's behalf.
    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        error = "is a remote URL; send the image as base64 or a data URL";
        return "";
    }
    if (value.rfind("data:", 0) == 0) {
        const size_t comma = value.find(',');
        if (comma == std::string::npos || value.substr(0, comma).find(";base64") == std::string::npos) {
            error = "is a data URL without a base64 payload";
            return "";
        }
        value = value.substr(comma + 1);
    }
    if (!is_detectable_image(utils::JsonUtils::base64_decode(value.substr(0, 16)))) {
        error = "is not a PNG, JPEG, BMP or WebP image";
        return "";
    }
    return value;
}

class ImagesDetectionsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_detections";
        s.methods = {"POST"};
        s.paths = {"images/detections"};
        s.summary = "Detect objects in images";
        s.modality = "image -> labeled bounding boxes";
        s.experimental = true;
        s.description =
            "Finds objects in one or more images and returns a label, a score and a bounding box "
            "for each. OpenAI has no detection endpoint, so this one follows its `images/*` "
            "family: the same image inputs, a `list` envelope with one `data` entry per input "
            "image, and OpenAI-shaped errors. It serves `ryzenai-vision` models, which run ONNX "
            "detectors on the Ryzen AI NPU.";
        s.notes = {
            "A malformed request, such as one whose `image` is not an image, is rejected before "
            "any model loads.",
            "### Setup\n\n"
            "The `ryzenai-vision` backend runs on a separately installed "
            "[Ryzen AI Software](https://ryzenai.docs.amd.com/en/latest/inst.html) environment. "
            "Point Lemonade at it once with `lemonade config set "
            "ryzenaivision.npu_bin=<env dir or its python>`; on Linux, starting `lemond` from an "
            "activated Ryzen AI venv also works through `RYZEN_AI_INSTALLATION_PATH`.\n\n"
            "The first load of a model compiles it for the NPU, about 15 minutes for YOLO26s on a "
            "Strix Halo machine. The result is cached under `<cache_dir>/ryzenai-vision/npu-cache` "
            "and later loads take about a second. Like `ryzenai-llm`, loading a detection model "
            "unloads the other NPU models; GPU and CPU models stay loaded.",
            "### Supported Models\n\n"
            "Ultralytics YOLO26 and YOLOv10 end-to-end exports (`[1, N, 6]` output), YOLOv8 to "
            "YOLO12 exports (`[1, 4 + C, A]`, NMS is applied by the server), and RT-DETR-style "
            "`logits` + `pred_boxes` exports with sigmoid scores, such as "
            "`onnx-community/yolo26s-ONNX`. Register another one as a custom model:\n\n"
            "```bash\n"
            "lemonade pull user.YOLO26n --checkpoint main onnx-community/yolo26n-ONNX:onnx/model.onnx "
            "--recipe ryzenai-vision\n"
            "```\n\n"
            "Class names come from the model's `config.json` (`id2label`) or the Ultralytics "
            "`names` metadata in the ONNX file, falling back to the 80 COCO classes.",
        };
        s.args = {kModel, kImage, kConfidence, kClasses, kMaxDetections};

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
                        "required": ["object", "index", "width", "height", "detections"],
                        "properties": {
                            "object": {"const": "image.detections", "description": "Always `image.detections`."},
                            "index": {"type": "integer", "description": "Position of the image in the request."},
                            "width": {"type": "integer", "description": "Width of the decoded image in pixels."},
                            "height": {"type": "integer", "description": "Height of the decoded image in pixels."},
                            "detections": {
                                "type": "array",
                                "description": "The detections, highest score first.",
                                "items": {
                                    "type": "object",
                                    "required": ["object", "label", "class_id", "score", "box"],
                                    "properties": {
                                        "object": {"const": "detection", "description": "Always `detection`."},
                                        "label": {"type": "string", "description": "Class name from the model's label map."},
                                        "class_id": {"type": "integer", "description": "Class index in the model's label map."},
                                        "score": {"type": "number", "description": "Confidence, in `[0, 1]`."},
                                        "box": {
                                            "type": "object",
                                            "description": "Corners in pixels of the original image: `x1,y1` top-left, `x2,y2` bottom-right.",
                                            "required": ["x1", "y1", "x2", "y2"],
                                            "properties": {
                                                "x1": {"type": "number"},
                                                "y1": {"type": "number"},
                                                "x2": {"type": "number"},
                                                "y2": {"type": "number"}
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
                    "required": ["images", "detections", "inference_ms"],
                    "properties": {
                        "images": {"type": "integer", "description": "Images analyzed."},
                        "detections": {"type": "integer", "description": "Detections returned across all images."},
                        "inference_ms": {"type": "number", "description": "Time spent decoding, preprocessing and running the model for all images."}
                    }
                },
                "unresolved_classes": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Requested `classes` that match no class of the model; present only when there are some."
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "YOLO26s-Detection-NPU",
            "image": "data:image/png;base64,@fixtures/image.png",
            "confidence": 0.1
        })");
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kImageInvalid,
                                 kNoSoleDetector, kBackendRejected, kModelUnloaded, kBackendError,
                                 kMalformedBackendResponse, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return kModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/detections" << std::endl;

        const json& raw = req.arg(kImage);
        const json inputs = raw.is_array() ? raw : json::array({raw});
        if (inputs.empty() || inputs.size() > kMaxImages) {
            write_openai_error(res, kImageInvalid,
                               "'image' must hold 1 to " + std::to_string(kMaxImages) + " images");
            return false;
        }
        json images = json::array();
        for (size_t i = 0; i < inputs.size(); ++i) {
            std::string error;
            std::string payload = image_payload(inputs[i], error);
            if (payload.empty()) {
                write_openai_error(res, kImageInvalid, "image[" + std::to_string(i) + "] " + error);
                return false;
            }
            images.push_back(std::move(payload));
        }
        req.body.erase(kImage.name);
        req.body["images"] = std::move(images);

        if (req.has(kMaxDetections)) {
            // The validator accepts 2.0 as an integer; the backend expects 2.
            req.body[kMaxDetections.name] =
                static_cast<long long>(req.arg(kMaxDetections).get<double>());
        }
        if (req.has(kClasses) && req.arg(kClasses).is_string()) {
            req.body[kClasses.name] = json::array({req.arg(kClasses)});
        }

        // The router requires a model on every request, so an omitted one is resolved
        // here, which is only possible when exactly one detector is loaded.
        if (!req.has(kModel)) {
            req.model = ctx_.router->get_sole_loaded_model_of_type(ModelType::DETECTION);
            if (req.model.empty()) {
                write_openai_error(res, kNoSoleDetector,
                    "No 'model' specified and no single detection model is loaded "
                    "(load one, or name it in the request)");
                return false;
            }
            req.body[kModel.name] = req.model;
            req.model_already_loaded = true;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        const size_t image_count = req.body["images"].size();
        auto response = ctx_.router->image_detections(req.body);
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }

        // The envelope pins the public API shape; the backend subprocess's raw
        // output is not the contract.
        if (!response.contains("data") || !response["data"].is_array() ||
            response["data"].size() != image_count) {
            write_openai_error(res, kMalformedBackendResponse,
                               "Detection backend returned an unexpected response", "server_error");
            return;
        }
        json data = json::array();
        size_t total = 0;
        for (const auto& item : response["data"]) {
            json detections = json::array();
            for (const auto& det : item.value("detections", json::array())) {
                detections.push_back({
                    {"object", "detection"},
                    {"label", det.value("label", "")},
                    {"class_id", det.value("class_id", -1)},
                    {"score", det.value("score", 0.0)},
                    {"box", det.value("box", json::object())},
                });
            }
            total += detections.size();
            data.push_back({
                {"object", "image.detections"},
                {"index", item.value("index", static_cast<int>(data.size()))},
                {"width", item.value("width", 0)},
                {"height", item.value("height", 0)},
                {"detections", std::move(detections)},
            });
        }
        json enveloped = {
            {"object", "list"},
            {"created", static_cast<long long>(std::time(nullptr))},
            {"model", req.model.empty() ? ctx_.router->get_loaded_model() : req.model},
            {"data", std::move(data)},
            {"usage", {
                {"images", image_count},
                {"detections", total},
                {"inference_ms", response.value("inference_ms", 0.0)},
            }},
        };
        if (response.contains("unresolved_classes") && response["unresolved_classes"].is_array() &&
            !response["unresolved_classes"].empty()) {
            enveloped["unresolved_classes"] = response["unresolved_classes"];
        }
        res.set_content(enveloped.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_detections_route(ServerContext& ctx) {
    return std::make_unique<ImagesDetectionsRoute>(ctx);
}

} // namespace lemon
