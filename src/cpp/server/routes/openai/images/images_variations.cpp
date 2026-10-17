#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const ordered_json kStringSchema = {{"type", "string"}};

const RouteArg kImage{"image", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Available,
                      "The source image (PNG).", {"image[]"}};
const RouteArg kPrompt{"prompt", ArgIn::Form, kStringSchema, false, Support::NotAvailable,
                       "Ignored; the variation depends only on the image."};
const RouteArg kSize{"size", ArgIn::Form, kStringSchema, false, Support::Available,
                     "Output size as `WIDTHxHEIGHT`. Defaults to the model's own, else `512x512`."};

class ImagesVariationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_variations";
        s.methods = {"POST"};
        s.paths = {"images/variations"};
        s.summary = "Generate a variation of an image";
        s.modality = "image -> varied image";
        s.description =
            "Generates a variation of a source image, sent as `multipart/form-data`. CPU "
            "inference takes minutes per image; a GPU backend is much faster.";
        s.notes = {
            "### Create Variations with the OpenAI Python Client\n"
            "\n"
            "=== \"Python (OpenAI client)\"\n"
            "\n"
            "    ```python\n"
            "    from openai import OpenAI\n"
            "    client = OpenAI(base_url=\"http://localhost:13305/v1\", api_key=\"not-needed\")\n"
            "    with open(\"source_image.png\", \"rb\") as image_file:\n"
            "        response = client.images.create_variation(\n"
            "            model=\"Flux-2-Klein-4B\",\n"
            "            image=image_file,\n"
            "            size=\"512x512\",\n"
            "            n=1,\n"
            "        )\n"
            "    import base64\n"
            "    image_data = base64.b64decode(response.data[0].b64_json)\n"
            "    open(\"variation.png\", \"wb\").write(image_data)\n"
            "    ```",
        };
        s.args = {
            model_loading::kFormModel, kImage, kPrompt, kSize, openai::image_uploads::kN,
            openai::image_uploads::kResponseFormat, openai::image_uploads::kUser
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Flux-2-Klein-4B",
            "size": "512x512",
            "n": 1,
            "response_format": "b64_json",
            "image": "@fixtures/image.png"
        })");
        s.examples = {example};
        s.errors = join_errors({{kFormNotMultipart, kArgMissing, kArgInvalid, kImageBackendError},
                                attach_form_image_errors(), image_route_exception_errors(),
                                ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Form;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kFormModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/variations" << std::endl;
        return attach_form_image(req, res, kImage);
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_variations(req.body);
        write_image_backend_response(response, res);
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "server_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_images_variations_route(ServerContext& ctx) {
    return std::make_unique<ImagesVariationsRoute>(ctx);
}

} // namespace lemon
