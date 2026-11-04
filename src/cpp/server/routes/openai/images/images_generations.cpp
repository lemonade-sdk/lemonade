#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/openai_args.h"
#include "lemon/server/shared_args.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

} // namespace

namespace openai::images {

const RouteArg kPrompt{"prompt", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                       "The text description of the image to generate."};
const RouteArg kSize{"size", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                     "Image size as `WIDTHxHEIGHT`, such as `256x256`. Defaults to the model's own, else `512x512`."};
const RouteArg kN{"n", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                  "Number of images to generate. Defaults to `1`."};
const RouteArg kResponseFormat{"response_format", ArgIn::JsonBody, {{"const", "b64_json"}}, false, Support::Partial,
                               "Only `b64_json`, a base64-encoded image, is supported.", {}, true};
const RouteArg kCfgScale{"cfg_scale", ArgIn::JsonBody, {{"type", "number"}}, false, Support::Available,
                         "Classifier-free guidance scale. Defaults to the model's own, else `7.0`."};
const RouteArg kSeed{"seed", ArgIn::JsonBody, {{"type", "integer"}}, false, Support::Available,
                     "Random seed for reproducible output; a negative value picks a random seed. Defaults "
                     "to the backend's fixed seed."};

} // namespace openai::images

namespace {

using namespace openai::images;

class ImagesGenerationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_generations";
        s.methods = {"POST"};
        s.paths = {"images/generations"};
        s.summary = "Generate an image from a text prompt";
        s.modality = "prompt -> image";
        s.description =
            "Generates an image from a text prompt. CPU inference takes minutes per image; a GPU backend is much faster.";
        s.args = {
            kPrompt, model_loading::kModel, kSize, kN, kResponseFormat, inference::kSteps,
            kCfgScale, kSeed
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
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid, kImageBackendError},
                                image_route_exception_errors(), ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kModel; }

    bool validate(RouteRequest&, httplib::Response&) override {
        LOG(INFO, "Server") << "POST /api/v1/images/generations" << std::endl;
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_generations(req.body);
        write_image_backend_response(response, res);
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "internal_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_images_generations_route(ServerContext& ctx) {
    return std::make_unique<ImagesGenerationsRoute>(ctx);
}

} // namespace lemon
