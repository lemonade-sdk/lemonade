#include <ctime>

#include <lemon/utils/aixlog.hpp>

#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"
#include "lemon/server_capabilities.h"

namespace lemon {

namespace {

using json = nlohmann::json;

const RouteError kNotUpscalingModel{400, "", "`model` has no `upscaling` label"};
const RouteError kUnknownUpscaleModel{
    404, "", "`model` names no model in the registry, or downloading it fails"};
const RouteError kRecipeCannotUpscale{400, "", "`model`'s recipe cannot upscale"};
const RouteError kUpscaleFailed{500, "", "The backend produces no upscaled image"};

// Upscaling runs the model once through its backend's command-line tool instead of a
// loaded server, so this route never auto-loads and is not a ModelRoute.
const RouteArg kImage{"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "Base64-encoded PNG image to upscale."};
const RouteArg kModel{"model", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                      "The upscaling model to use, such as `RealESRGAN-x4plus` or `Remacri-4x-TheNoise`."};

class ImagesUpscaleRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_upscale";
        s.methods = {"POST"};
        s.paths = {"images/upscale"};
        s.summary = "Upscale an image";
        s.modality = "image + ESRGAN model -> upscaled image";
        s.description =
            "Upscales an image with an upscaling model such as `RealESRGAN-x4plus`. The model sets "
            "the upscale factor, which its name usually states.";
        s.notes = {
            "The model is downloaded on first use.",
            "### Upscale a Generated Image\n"
            "\n"
            "To upscale an image from [`POST /v1/images/generations`](#post-v1imagesgenerations), "
            "pass its `b64_json` as `image`.",
        };
        s.args = {kImage, kModel};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.schema["properties"]["created"]["description"] =
            "Unix timestamp of when the upscaled image was generated.";
        response.schema["properties"]["data"]["description"] = "The upscaled image.";
        response.schema["properties"]["data"]["items"]["properties"]["b64_json"]["description"] =
            "Base64-encoded PNG of the upscaled image.";
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"image", "@fixtures/image.png"}, {"model", "RealESRGAN-x4plus"}};
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgMissing, kArgInvalid,
                                 kNotUpscalingModel, kUnknownUpscaleModel, kRecipeCannotUpscale,
                                 kUpscaleFailed},
                                image_route_exception_errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager& model_manager = *ctx_.model_manager;
        try {
            LOG(INFO, "Server") << "POST /api/v1/images/upscale" << std::endl;

            std::string upscale_model_name = req.arg(kModel).get<std::string>();

            ModelInfo info;
            try {
                info = model_manager.get_model_info(upscale_model_name);

                if (!lemon::has_label(info.labels, "upscaling")) {
                    write_openai_error(res, kNotUpscalingModel,
                        "Upscale model is not labeled 'upscaling': " + upscale_model_name);
                    return;
                }

                if (!model_manager.is_model_downloaded(upscale_model_name)) {
                    LOG(INFO, "Server") << "Upscale model not cached, downloading from its remote registry..." << std::endl;
                    model_manager.download_registered_model(info, true);
                    LOG(INFO, "Server") << "Upscale model download complete: " << upscale_model_name << std::endl;
                    info = model_manager.get_model_info(upscale_model_name);
                }

            } catch (const std::exception& e) {
                write_openai_error(res, kUnknownUpscaleModel,
                                   "Upscale model not found: " + upscale_model_name);
                return;
            }

            std::string b64_image = req.arg(kImage).get<std::string>();

            backends::BackendContext context;
            context.log_level = ctx_.config->log_level();
            context.model_manager = ctx_.model_manager;
            context.backend_manager = ctx_.backend_manager;
            context.cloud_registry = ctx_.cloud_registry;
            context.model_info = &info;
            auto server = backends::create_server(info.recipe, context);
            if (!server || !supports_capability<IUpscaleServer>(server.get())) {
                write_openai_error(res, kRecipeCannotUpscale,
                                   "Upscale is not supported by recipe: " + info.recipe);
                return;
            }
            auto* upscale_server = dynamic_cast<IUpscaleServer*>(server.get());
            std::string upscaled = upscale_server->upscale_via_cli(b64_image, info.resolved_path("main"));

            if (upscaled.empty()) {
                write_openai_error(res, kUpscaleFailed, "Upscale failed", "server_error");
                return;
            }

            json response;
            response["created"] = static_cast<long long>(std::time(nullptr));
            response["data"] = json::array();
            response["data"].push_back({{"b64_json", upscaled}});
            res.set_content(response.dump(), "application/json");

        } catch (const std::exception& e) {
            write_image_route_exception(e, res, "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_upscale_route(ServerContext& ctx) {
    return std::make_unique<ImagesUpscaleRoute>(ctx);
}

} // namespace lemon
