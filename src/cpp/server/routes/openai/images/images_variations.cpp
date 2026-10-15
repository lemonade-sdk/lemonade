#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kImageBackendError{
    500, "", "The backend reports an error, for example when `model` cannot make image variations"};

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
        const ordered_json text = {{"type", "string"}};
        s.args = {
            {"model", ArgIn::Form, text, true, Support::Available,
             "The image model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`)."},
            {"image", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Available,
             "The source image (PNG)."},
            {"prompt", ArgIn::Form, text, false, Support::NotAvailable,
             "Ignored; the variation depends only on the image."},
            {"size", ArgIn::Form, text, false, Support::Available,
             "Output size as `WIDTHxHEIGHT`. Defaults to the model's own, else `512x512`."},
            {"n", ArgIn::Form, {{"type", "integer"}}, false, Support::Available,
             "Number of images to generate, from `1` to `10`. "
             "Defaults to `1`."},
            {"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
             "Only `b64_json`, a base64-encoded image, is supported."},
            {"user", ArgIn::Form, text, false, Support::NotAvailable,
             "Accepted for OpenAI compatibility and ignored."},
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
        s.errors = attach_form_image_errors();
        s.errors.push_back(kImageBackendError);
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
            res.status = kImageBackendError.status;
        }
        res.set_content(response.dump(), "application/json");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "server_error");
    }
    std::vector<RouteError> exception_errors() const override {
        return image_route_exception_errors();
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_variations_route(ServerContext& ctx) {
    return std::make_unique<ImagesVariationsRoute>(ctx);
}

} // namespace lemon
