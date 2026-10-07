#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"

namespace lemon {

namespace {

using json = nlohmann::json;

class ImagesGenerationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_generations";
        s.methods = {"POST"};
        s.paths = {"images/generations"};
        s.summary = "Image Generation";
        s.description = "Generates an image from a text prompt, loading the model on first use.";
        s.notes = {
            "**Performance:** CPU inference takes about 4 to 5 minutes per image. GPU (ROCm) is "
            "significantly faster.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Diffusion model, e.g. `SD-Turbo` or `Krea-2-Turbo`."},
            {"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Text description of the image to generate."},
            {"size", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Image size as `WIDTHxHEIGHT`, e.g. `512x512` or `256x256`. Defaults to `512x512`."},
            {"n", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Partial,
             "Number of images to generate. Only `1` is supported."},
            {"response_format", ArgIn::JsonBody, {{"const", "b64_json"}}, false, Support::Partial,
             "Only `b64_json` (a base64-encoded image) is supported."},
            {"steps", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Number of inference steps. SD-Turbo works well with 4. The default varies by "
             "model."},
            {"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance scale. SD-Turbo uses low values (about 1.0). The default "
             "varies by model."},
            {"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
             "Random seed for reproducibility. A random seed is used when omitted."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = json::parse(R"({
            "model": "SD-Turbo",
            "prompt": "A serene mountain landscape at sunset",
            "size": "256x256",
            "steps": 4,
            "response_format": "b64_json"
        })");
        s.responses = {response};
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
