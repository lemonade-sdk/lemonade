#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class Generations3dRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.3d_generations";
        s.methods = {"POST"};
        s.paths = {"3d/generations"};
        s.summary = "Generate a textured 3D mesh (GLB) from an image";
        s.experimental = true;
        s.description =
            "Reconstructs a textured 3D mesh from an image and returns it as a glTF-binary "
            "(`.glb`) file. TRELLIS models, such as `TRELLIS-3D`, serve it.";
        s.notes = {
            "This is a Lemonade extension; OpenAI has no 3D endpoint.",
            "**Performance:** reconstruction runs on the GPU (Vulkan, ROCm or CUDA) and takes "
            "minutes; higher resolutions take longer.",
            "A failure is answered with a JSON `error` object instead of a mesh: `400` for an "
            "invalid request, checked before the model loads, `404` for an unknown model, `500` "
            "when the backend reports an error, and `502` when the backend produces no output.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "3D-generation model, e.g. `TRELLIS-3D`; loaded on first use."},
            {"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Base64-encoded input image, optionally as a `data:` URL. PNG, JPEG, BMP and GIF "
             "are accepted."},
            {"resolution", ArgIn::JsonBody,
             {{"enum", json::array({512, 1024, 1536, "512", "1024", "1536"})}}, false, Support::Available,
             "Cascade resolution: `512`, `1024` or `1536`. Defaults to `512`."},
            {"bg_removal", ArgIn::JsonBody, {{"enum", json::array({"threshold", "birefnet"})}},
             false, Support::Available,
             "Background removal mode: `threshold`, or `birefnet` for photos with real "
             "backgrounds."},
            {"uv", ArgIn::JsonBody, {{"enum", json::array({"xatlas", "box"})}}, false, Support::Available,
             "UV atlas method. `xatlas` (default) runs a full UV unwrap that gives every face "
             "its own atlas space: the best quality, but its chart computation grows faster "
             "than the face count. `box` is a faster 6-plane projection with occlusion-aware "
             "bucket assignment and depth-tested rasterization; small texture artifacts remain "
             "possible in concave regions."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed, for reproducible output."},
            {"response_format", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Output encoding. Only formats the backend produces natively are accepted "
             "(currently `glb`); any other is answered with `400`. Defaults to `glb`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Binary;
        response.example = json::parse(R"({
            "model": "TRELLIS-3D",
            "image": "@fixtures/image.png",
            "resolution": 512,
            "seed": 42
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    // All cheap validation runs before the load, so a malformed request can never
    // trigger a multi-gigabyte model load.
    bool validate(RouteRequest& req, httplib::Response& res) override {
        const json& request_json = req.body;
        auto reject = [&res](const std::string& message) {
            write_openai_error(res, 400, message);
            return false;
        };
        if (!request_json.contains("model") || !request_json["model"].is_string()) {
            return reject("Missing or non-string 'model' field in request");
        }
        if (!request_json.contains("image") || !request_json["image"].is_string()) {
            return reject("Missing or non-string 'image' field in request (base64-encoded input image)");
        }
        {
            std::string image = request_json["image"].get<std::string>();
            if (image.rfind("data:", 0) == 0) {
                const auto comma = image.find(',');
                if (comma == std::string::npos) {
                    return reject("'image' data URL is missing its base64 payload");
                }
                image = image.substr(comma + 1);
            }
            if (image.empty() || image.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=\r\n") !=
                    std::string::npos) {
                return reject("'image' must be base64 data or a base64 data: URL");
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
                return reject("'image' is not a supported format (expected PNG, JPEG, BMP, or GIF)");
            }
        }
        std::string response_format = "glb";
        if (request_json.contains("response_format")) {
            if (!request_json["response_format"].is_string()) {
                return reject("'response_format' must be a string");
            }
            response_format = request_json["response_format"].get<std::string>();
        }
        // TODO: convert from a natively supported format instead of rejecting.
        if (response_format != "glb") {
            return reject("response_format '" + response_format +
                          "' is not supported (supported: glb)");
        }
        if (request_json.contains("resolution")) {
            const auto& r = request_json["resolution"];
            const std::string v = r.is_string() ? r.get<std::string>()
                : (r.is_number_integer() ? std::to_string(r.get<int>()) : "");
            if (v != "512" && v != "1024" && v != "1536") {
                return reject("'resolution' must be 512, 1024, or 1536");
            }
        }
        if (request_json.contains("bg_removal")) {
            const auto& b = request_json["bg_removal"];
            if (!b.is_string() || (b != "threshold" && b != "birefnet")) {
                return reject("'bg_removal' must be 'threshold' or 'birefnet'");
            }
        }
        if (request_json.contains("seed") && !request_json["seed"].is_number_integer()) {
            return reject("'seed' must be an integer");
        }
        if (request_json.contains("uv")) {
            const auto& u = request_json["uv"];
            if (!u.is_string() || (u != "box" && u != "xatlas")) {
                return reject("'uv' must be 'box' or 'xatlas'");
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
        write_openai_error(res, 500, error.what(), "internal_error");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        LOG(ERROR, "Server") << "ERROR in handle_3d_generations: " << error.what() << std::endl;
        write_openai_error(res, 500, error.what(), "internal_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_3d_generations_route(ServerContext& ctx) {
    return std::make_unique<Generations3dRoute>(ctx);
}

} // namespace lemon
