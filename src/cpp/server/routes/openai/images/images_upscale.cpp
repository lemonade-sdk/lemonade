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

// Upscaling runs the model once through its backend's command-line tool instead of a
// loaded server, so this route never auto-loads and is not a ModelRoute.
class ImagesUpscaleRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_upscale";
        s.methods = {"POST"};
        s.paths = {"images/upscale"};
        s.summary = "Image Upscaling";
        s.description =
            "Upscales a base64-encoded image with a Real-ESRGAN model. The upscale factor depends "
            "on the model and is usually in its name.";
        s.notes = {
            "Unlike [`/v1/images/edits`](#post-v1imagesedits) and "
            "[`/v1/images/variations`](#post-v1imagesvariations), this endpoint takes a JSON body, "
            "with the image as a base64 string.",
            "The model must carry the `upscaling` label and is downloaded on first use. A missing "
            "`image` or `model`, or a model without the label, answers `400`; an unknown model "
            "answers `404`; a failed upscale answers `500`. Each error body is "
            "`{\"error\": {\"message\": ..., \"type\": ...}}`.",
        };
        s.args = {
            {"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Base64-encoded PNG image to upscale."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Upscaling model, e.g. `RealESRGAN-x4plus` or `Remacri-4x-TheNoise`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = {{"image", "@fixtures/image.png"}, {"model", "RealESRGAN-x4plus"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager& model_manager = *ctx_.model_manager;
        try {
            LOG(INFO, "Server") << "POST /api/v1/images/upscale" << std::endl;

            auto request_json = json::parse(req.http.body);

            if (!request_json.contains("image") || !request_json["image"].is_string()) {
                write_openai_error(res, 400, "Missing 'image' field (base64 encoded)");
                return;
            }

            std::string upscale_model_name = request_json.value("model", "");
            if (upscale_model_name.empty()) {
                write_openai_error(res, 400, "Missing 'model' field");
                return;
            }

            ModelInfo info;
            try {
                info = model_manager.get_model_info(upscale_model_name);

                if (!lemon::has_label(info.labels, "upscaling")) {
                    write_openai_error(res, 400,
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
                write_openai_error(res, 404, "Upscale model not found: " + upscale_model_name);
                return;
            }

            std::string b64_image = request_json["image"].get<std::string>();

            backends::BackendContext context;
            context.log_level = ctx_.config->log_level();
            context.model_manager = ctx_.model_manager;
            context.backend_manager = ctx_.backend_manager;
            context.cloud_registry = ctx_.cloud_registry;
            context.model_info = &info;
            auto server = backends::create_server(info.recipe, context);
            if (!server || !supports_capability<IUpscaleServer>(server.get())) {
                write_openai_error(res, 400, "Upscale is not supported by recipe: " + info.recipe);
                return;
            }
            auto* upscale_server = dynamic_cast<IUpscaleServer*>(server.get());
            std::string upscaled = upscale_server->upscale_via_cli(b64_image, info.resolved_path("main"));

            if (upscaled.empty()) {
                write_openai_error(res, 500, "Upscale failed", "server_error");
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
