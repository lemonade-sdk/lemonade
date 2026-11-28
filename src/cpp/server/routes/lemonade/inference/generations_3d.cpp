#include <lemon/utils/aixlog.hpp>

#include "lemon/models/model_info.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kMalformedImage{
    400, "", "`image` is not base64 data, or is a `data:` URL without a base64 payload"};
const RouteError kUnsupportedImage{400, "", "`image` is not a PNG, JPEG, BMP or GIF"};

const RouteArg kImage{"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "Base64-encoded PNG, JPEG, BMP or GIF image, optionally as a `data:` URL."};
const RouteArg kResolution{"resolution", ArgIn::JsonBody,
                           {{"enum", json::array({512, 1024, 1536, "512", "1024", "1536"})}}, false, Support::Available,
                           "TRELLIS cascade resolution: `512`, `1024` or `1536`. Defaults to `512`."};
const RouteArg kOctreeResolution{"octree_resolution", ArgIn::JsonBody,
                                 {{"type", "integer"}, {"minimum", 8}, {"maximum", 768}}, false, Support::Available,
                                 "TheNoise mesh extraction grid resolution (8-768). Defaults to the model's "
                                 "`octree_resolution` recipe option, else the backend default."};
const RouteArg kTriangles{"triangles", ArgIn::JsonBody,
                          {{"type", "number"}, {"minimum", 0.001}, {"maximum", 1.0}}, false, Support::Available,
                          "TheNoise fraction of triangles to retain (0.001-1.0). Defaults to the model's "
                          "`triangles` recipe option, else 0.05; use 1.0 for the raw detail level."};
const RouteArg kMeshSteps{"steps", ArgIn::JsonBody, {{"type", "integer"}, {"minimum", 1}}, false, Support::Available,
                         "TheNoise mesh denoising steps. Defaults to the model setting."};
const RouteArg kBgRemoval{"bg_removal", ArgIn::JsonBody, {{"enum", json::array({"threshold", "birefnet"})}},
                          false, Support::Available,
                          "TRELLIS background removal mode: `threshold` or `birefnet`. Use `birefnet` for photos with "
                          "real backgrounds. Defaults to the backend's own."};
const RouteArg kUv{"uv", ArgIn::JsonBody, {{"enum", json::array({"xatlas", "box"})}}, false, Support::Available,
                   "TRELLIS UV atlas method. `xatlas` unwraps every face into its own atlas space for the best "
                   "texture quality, but slows down on large meshes. `box` is a faster six-plane "
                   "projection that can leave small artifacts in concave regions. Defaults to `xatlas`."};
const RouteArg kResponseFormat{"response_format", ArgIn::JsonBody, {{"enum", json::array({"glb", "stl", "obj"})}},
                               false, Support::Available,
                               "Output encoding: `glb` (default), or `stl` or `obj` (TheNoise only).", {"output_format"}};

class Generations3dRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.3d_generations";
        s.methods = {"POST"};
        s.paths = {"3d/generations"};
        s.summary = "Generate a 3D mesh from an image";
        s.experimental = true;
        s.description =
            "Generates a mesh from an image. TRELLIS produces textured GLB meshes; "
            "TheNoise Hunyuan3D 2.1 produces geometry without textures. This is a Lemonade extension.";
        s.notes = {
            "Reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes minutes; higher "
            "`resolution` (TRELLIS) or `octree_resolution` (TheNoise) values take longer.",
            "Backend-specific parameters are only used by the corresponding backend.",
            "TheNoise forwards additional JSON fields to its native mesh API, such as "
            "`smooth` and `floaters` (0.0-1.0).",
            "Errors carry a JSON `error` object.",
        };
        s.args = {
            model_loading::kModel, kImage, kResolution, kBgRemoval, kUv, kOctreeResolution,
            kTriangles, kMeshSteps, inference::kSeed, kResponseFormat
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
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kMalformedImage,
                                 kUnsupportedImage, kNoOutputError,
                                 kBackendError, kUnexpectedException},
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    // All cheap validation runs before the load, so a malformed request can never
    // trigger a multi-gigabyte model load.
    bool validate(RouteRequest& req, httplib::Response& res) override {
        auto reject = [&res](const RouteError& error, const std::string& message) {
            write_openai_error(res, error, message);
            return false;
        };
        std::string image = req.arg(kImage).get<std::string>();
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
        if (req.value(kResponseFormat, "glb") != "glb") {
            if (auto info = ctx_.router->try_get_model_info(req.model); info && info->recipe != "thenoise") {
                return reject(kArgInvalid, "STL and OBJ output are only supported by TheNoise");
            }
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/3d/generations" << std::endl;

        Router* router = ctx_.router;
        const auto format = req.value(kResponseFormat, "glb");
        const char* mime = format == "stl" ? "model/stl" : format == "obj" ? "model/obj" : "model/gltf-binary";
        serve_media_or_error(res, mime, [router, &req](httplib::DataSink& sink) {
            router->model_3d_generations(req.body, sink);
        });
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in handle_3d_generations: " << error.what() << std::endl;
        write_openai_error(res, kUnexpectedException, error.what(), "internal_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_3d_generations_route(ServerContext& ctx) {
    return std::make_unique<Generations3dRoute>(ctx);
}

} // namespace lemon
