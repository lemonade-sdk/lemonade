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
        s.modality = "image + ESRGAN model -> upscaled image";
        s.description =
            "Upscales an image with a Real-ESRGAN model. The request is JSON with the image "
            "base64-encoded. The model sets the upscale factor, which its name usually states.";
        s.notes = {
            "The model must carry the `upscaling` label and is downloaded on first use.",
            "A missing `image` or `model` answers `400`, an unknown model `404`, and a failed "
            "upscale `500`, each with an `error` object.",
            "### Upscale a Generated Image\n"
            "\n"
            "A typical workflow is to generate an image first, then upscale it:\n"
            "\n"
            "=== \"Bash\"\n"
            "\n"
            "    ```bash\n"
            "    # Step 1: Generate an image and save the base64 response\n"
            "    RESPONSE=$(curl -s -X POST http://localhost:13305/v1/images/generations \\\n"
            "      -H \"Content-Type: application/json\" \\\n"
            "      -d '{\n"
            "            \"model\": \"SD-Turbo\",\n"
            "            \"prompt\": \"A serene mountain landscape at sunset\",\n"
            "            \"size\": \"512x512\",\n"
            "            \"steps\": 4,\n"
            "            \"response_format\": \"b64_json\"\n"
            "          }')\n"
            "\n"
            "    # Step 2: Build the upscale JSON payload and pipe it to curl via stdin\n"
            "    # (base64 images are too large for command-line interpolation)\n"
            "    echo \"$RESPONSE\" | python3 -c \"\n"
            "    import sys, json\n"
            "    b64 = json.load(sys.stdin)['data'][0]['b64_json']\n"
            "    print(json.dumps({'image': b64, 'model': 'RealESRGAN-x4plus'}))\n"
            "    \" | curl -X POST http://localhost:13305/v1/images/upscale \\\n"
            "      -H \"Content-Type: application/json\" \\\n"
            "      -d @-\n"
            "    ```\n"
            "\n"
            "=== \"PowerShell\"\n"
            "\n"
            "    ```powershell\n"
            "    # Step 1: Generate an image\n"
            "    $genResponse = Invoke-WebRequest `\n"
            "      -Uri \"http://localhost:13305/v1/images/generations\" `\n"
            "      -Method POST `\n"
            "      -Headers @{ \"Content-Type\" = \"application/json\" } `\n"
            "      -Body '{\n"
            "        \"model\": \"SD-Turbo\",\n"
            "        \"prompt\": \"A serene mountain landscape at sunset\",\n"
            "        \"size\": \"512x512\",\n"
            "        \"steps\": 4,\n"
            "        \"response_format\": \"b64_json\"\n"
            "      }'\n"
            "\n"
            "    # Step 2: Extract the base64 image\n"
            "    $imageB64 = ($genResponse.Content | ConvertFrom-Json).data[0].b64_json\n"
            "\n"
            "    # Step 3: Upscale the image with Real-ESRGAN\n"
            "    $body = @{ image = $imageB64; model = \"RealESRGAN-x4plus\" } | ConvertTo-Json\n"
            "    Invoke-WebRequest `\n"
            "      -Uri \"http://localhost:13305/v1/images/upscale\" `\n"
            "      -Method POST `\n"
            "      -Headers @{ \"Content-Type\" = \"application/json\" } `\n"
            "      -Body $body\n"
            "    ```\n"
            "\n"
            "=== \"Python (requests)\"\n"
            "\n"
            "    ```python\n"
            "    import requests\n"
            "    import base64\n"
            "\n"
            "    BASE_URL = \"http://localhost:13305/api/v1\"\n"
            "\n"
            "    # Step 1: Generate an image\n"
            "    gen_response = requests.post(f\"{BASE_URL}/images/generations\", json={\n"
            "        \"model\": \"SD-Turbo\",\n"
            "        \"prompt\": \"A serene mountain landscape at sunset\",\n"
            "        \"size\": \"512x512\",\n"
            "        \"steps\": 4,\n"
            "        \"response_format\": \"b64_json\",\n"
            "    })\n"
            "    image_b64 = gen_response.json()[\"data\"][0][\"b64_json\"]\n"
            "\n"
            "    # Step 2: Upscale the image with Real-ESRGAN (512x512 -> 2048x2048)\n"
            "    upscale_response = requests.post(f\"{BASE_URL}/images/upscale\", json={\n"
            "        \"image\": image_b64,\n"
            "        \"model\": \"RealESRGAN-x4plus\",\n"
            "    })\n"
            "\n"
            "    # Step 3: Save the upscaled image to a file\n"
            "    upscaled_b64 = upscale_response.json()[\"data\"][0][\"b64_json\"]\n"
            "    with open(\"upscaled.png\", \"wb\") as f:\n"
            "        f.write(base64.b64decode(upscaled_b64))\n"
            "    ```",
        };
        s.args = {
            {"image", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Base64-encoded PNG image to upscale."},
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "The ESRGAN model to use (e.g., `RealESRGAN-x4plus`, `Remacri-4x-TheNoise`)."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.schema["properties"]["created"]["description"] =
            "Unix timestamp of when the upscaled image was generated";
        response.schema["properties"]["data"]["description"] = "Array containing the upscaled image";
        response.schema["properties"]["data"]["items"]["properties"]["b64_json"]["description"] =
            "Base64-encoded PNG of the upscaled image";
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"image", "@fixtures/image.png"}, {"model", "RealESRGAN-x4plus"}};
        s.examples = {example};
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
