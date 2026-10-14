#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class ImagesGenerationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_generations";
        s.methods = {"POST"};
        s.paths = {"images/generations"};
        s.summary = "Image Generation";
        s.modality = "prompt -> image";
        s.description =
            "Generates an image from a text prompt. CPU inference takes minutes per image; a GPU backend is much faster.";
        s.args = {
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The text description of the image to generate."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The diffusion model to use (e.g., `SD-Turbo`, `Krea-2-Turbo`)."},
            {"size", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "The size of the generated image. Format: `WIDTHxHEIGHT` (e.g., `512x512`, "
             "`256x256`). Default: `512x512`."},
            {"n", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of images to generate. Defaults to `1`."},
            {"response_format", ArgIn::JsonBody, {{"const", "b64_json"}}, false, Support::Partial,
             "Format of the response. Only `b64_json` (base64-encoded image) is supported."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of inference steps. SD-Turbo works well with 4 steps. Default varies by "
             "model."},
            {"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance scale. SD-Turbo uses low values (~1.0). Default varies by "
             "model."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducibility. If not specified, a random seed is used."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "SD-Turbo",
            "prompt": "A serene mountain landscape at sunset",
            "size": "512x512",
            "steps": 4,
            "response_format": "b64_json"
        })");
        s.examples = {example};
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/generations" << std::endl;
        if (!req.body.contains("prompt")) {
            write_openai_error(res, 400, "Missing 'prompt' field in request");
            return false;
        }
        if (req.body.contains("model")) {
            req.body["model"].get<std::string>();  // a non-string model fails before any load
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_generations(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Image generation backend error: " << response.dump() << std::endl;
            res.status = 500;
        }
        res.set_content(response.dump(), "application/json");
    }

    void write_invalid_body(const RouteRequest&, const std::exception& error,
                            httplib::Response& res) const override {
        write_exception(error, res);
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "internal_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_generations_route(ServerContext& ctx) {
    return std::make_unique<ImagesGenerationsRoute>(ctx);
}

} // namespace lemon
