#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalid3dBody{500, "", "The body is empty or is not valid JSON"};
const RouteError kInvalidModelField{400, "", "`model` is missing or not a string"};
const RouteError kMissingImage{400, "", "`image` is missing or not a string"};
const RouteError kMalformedImage{
    400, "", "`image` is not base64 data, or is a `data:` URL without a base64 payload"};
const RouteError kUnsupportedImage{400, "", "`image` is not a PNG, JPEG, BMP or GIF"};
const RouteError kInvalidResponseFormat{400, "", "`response_format` is not the string `glb`"};
const RouteError kInvalidResolution{400, "", "`resolution` is not 512, 1024 or 1536"};
const RouteError kInvalidBgRemoval{400, "", "`bg_removal` is not `threshold` or `birefnet`"};
const RouteError kInvalidSeed{400, "", "`seed` is not an integer"};
const RouteError kInvalidUv{400, "", "`uv` is not `box` or `xatlas`"};
const RouteError kGenerationException{
    500, "", "`model` is not a 3D model, or generation fails unexpectedly"};

class Generations3dRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.3d_generations";
        s.methods = {"POST"};
        s.paths = {"3d/generations"};
        s.summary = "Generate a textured 3D mesh from an image";
        s.experimental = true;
        s.description =
            "Generates a textured 3D mesh, as a glTF-binary (`.glb`) file, from an image. It serves "
            "TRELLIS models such as `TRELLIS-3D` and is a Lemonade extension.";
        s.notes = {
            "Reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes minutes; higher "
            "`resolution` values take longer.",
            "Errors carry a JSON `error` object.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The 3D-generation model."},
            {"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Base64-encoded PNG, JPEG, BMP or GIF image, optionally as a `data:` URL."},
            {"resolution", ArgIn::JsonBody,
             {{"enum", json::array({512, 1024, 1536, "512", "1024", "1536"})}}, false, Support::Available,
             "Cascade resolution: `512`, `1024` or `1536`. Defaults to `512`."},
            {"bg_removal", ArgIn::JsonBody, {{"enum", json::array({"threshold", "birefnet"})}},
             false, Support::Available,
             "Background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with "
             "real backgrounds. Defaults to the backend's own."},
            {"uv", ArgIn::JsonBody, {{"enum", json::array({"xatlas", "box"})}}, false, Support::Available,
             "UV atlas method. `xatlas` unwraps every face into its own atlas space for the best "
             "texture quality, but slows down on large meshes. `box` is a faster six-plane "
             "projection that can leave small artifacts in concave regions. Defaults to `xatlas`."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducibility. Defaults to the backend's own."},
            {"response_format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Output encoding. Only `glb`, the format the backend produces, is accepted. "
             "Defaults to `glb`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Binary;
        example.args = ordered_json::parse(R"({
            "model": "TRELLIS-3D",
            "image": "@fixtures/image.png",
            "resolution": 512,
            "seed": 42
        })");
        s.examples = {example};
        s.errors = join_errors({{kInvalid3dBody, kModelMissing, kInvalidModelField, kMissingImage,
                                 kMalformedImage, kUnsupportedImage, kInvalidResponseFormat,
                                 kInvalidResolution, kInvalidBgRemoval, kInvalidSeed, kInvalidUv,
                                 kNoOutputError, kBackendError, kGenerationException},
                                ModelLoader::errors()});
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    // All cheap validation runs before the load, so a malformed request can never
    // trigger a multi-gigabyte model load.
    bool validate(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;
        auto reject = [&res](const RouteError& error, const std::string& message) {
            write_openai_error(res, error, message);
            return false;
        };
        if (!request_json.contains("model") || !request_json["model"].is_string()) {
            return reject(kInvalidModelField, "Missing or non-string 'model' field in request");
        }
        if (!request_json.contains("image") || !request_json["image"].is_string()) {
            return reject(kMissingImage,
                          "Missing or non-string 'image' field in request (base64-encoded input image)");
        }
        {
            std::string image = request_json["image"].get<std::string>();
            if (image.rfind("data:", 0) == 0) {
                const auto comma = image.find(',');
                if (comma == std::string::npos) {
                    return reject(kMalformedImage, "'image' data URL is missing its base64 payload");
                }
                image = image.substr(comma + 1);
            }
            if (image.empty() || image.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=\r\n") !=
                    std::string::npos) {
                return reject(kMalformedImage, "'image' must be base64 data or a base64 data: URL");
            }
            // Sniffing needs only the first 12 bytes; base64 is block-aligned,
            // so decoding the first 16 characters is enough and avoids decoding
            // a multi-megabyte payload just to validate it.
            std::string head;
            for (char c : image) {
                if (c != '\r' && c != '\n') {
                    head += c;
                    if (head.size() == 16) break;
                }
            }
            if (!utils::sniff_image(utils::JsonUtils::base64_decode(head)).ok()) {
                return reject(kUnsupportedImage,
                              "'image' is not a supported format (expected PNG, JPEG, BMP, or GIF)");
            }
        }
        std::string response_format = "glb";
        if (request_json.contains("response_format")) {
            if (!request_json["response_format"].is_string()) {
                return reject(kInvalidResponseFormat, "'response_format' must be a string");
            }
            response_format = request_json["response_format"].get<std::string>();
        }
        // TODO: convert from a natively supported format instead of rejecting.
        if (response_format != "glb") {
            return reject(kInvalidResponseFormat, "response_format '" + response_format +
                                                  "' is not supported (supported: glb)");
        }
        if (request_json.contains("resolution")) {
            const auto& r = request_json["resolution"];
            const std::string v = r.is_string() ? r.get<std::string>()
                : (r.is_number_integer() ? std::to_string(r.get<int>()) : "");
            if (v != "512" && v != "1024" && v != "1536") {
                return reject(kInvalidResolution, "'resolution' must be 512, 1024, or 1536");
            }
        }
        if (request_json.contains("bg_removal")) {
            const auto& b = request_json["bg_removal"];
            if (!b.is_string() || (b != "threshold" && b != "birefnet")) {
                return reject(kInvalidBgRemoval, "'bg_removal' must be 'threshold' or 'birefnet'");
            }
        }
        if (request_json.contains("seed") && !request_json["seed"].is_number_integer()) {
            return reject(kInvalidSeed, "'seed' must be an integer");
        }
        if (request_json.contains("uv")) {
            const auto& u = request_json["uv"];
            if (!u.is_string() || (u != "box" && u != "xatlas")) {
                return reject(kInvalidUv, "'uv' must be 'box' or 'xatlas'");
            }
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/3d/generations" << std::endl;

        Router* router = ctx_.router;
        serve_media_or_error(res, "model/gltf-binary", [router, &req](httplib::DataSink& sink) {
            router->model_3d_generations(req.body, sink);
        });
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_openai_error(res, kInvalid3dBody, error.what(), "internal_error");
    }
    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in handle_3d_generations: " << error.what() << std::endl;
        write_openai_error(res, kGenerationException, error.what(), "internal_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_3d_generations_route(ServerContext& ctx) {
    return std::make_unique<Generations3dRoute>(ctx);
}

} // namespace lemon
