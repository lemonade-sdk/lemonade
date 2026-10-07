#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"

namespace lemon {

namespace {

using json = nlohmann::json;

class ImagesVariationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_variations";
        s.methods = {"POST"};
        s.paths = {"images/variations"};
        s.summary = "Image Variations";
        s.description =
            "Generates a variation of a source image, loading the model on first use. The request "
            "is `multipart/form-data`.";
        s.notes = {
            "Unlike [`/v1/images/edits`](#post-v1imagesedits), this takes no `prompt`; a prompt "
            "field is ignored and the variation follows the input image alone.",
            "**Performance:** CPU inference takes several minutes per image. GPU (ROCm) is "
            "significantly faster.",
        };
        const json text = {{"type", "string"}};
        s.args = {
            {"model", ArgIn::Form, text, true, Support::Available,
             "Diffusion model, e.g. `Flux-2-Klein-4B` or `SD-Turbo`."},
            {"image", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Available,
             "Source image (PNG). The field may also be named `image[]`."},
            {"size", ArgIn::Form, text, false, Support::Available,
             "Output size as `WIDTHxHEIGHT`, e.g. `512x512`. Defaults to `512x512`."},
            {"n", ArgIn::Form, {{"type", "integer"}}, false, Support::Partial,
             "Number of variations to generate, from `1` to `10`. Defaults to `1`; values "
             "outside the range answer `400`."},
            {"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
             "Only `b64_json` (a base64-encoded image) is supported."},
            {"user", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted but not forwarded to the backend."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = json::parse(R"({
            "model": "SD-Turbo",
            "image": "@fixtures/image.png",
            "size": "256x256"
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Form;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/variations" << std::endl;
        return attach_form_image(req, res);
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_variations(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Image variations backend error: " << response.dump() << std::endl;
            res.status = 500;
        }
        res.set_content(response.dump(), "application/json");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "server_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_variations_route(ServerContext& ctx) {
    return std::make_unique<ImagesVariationsRoute>(ctx);
}

} // namespace lemon
