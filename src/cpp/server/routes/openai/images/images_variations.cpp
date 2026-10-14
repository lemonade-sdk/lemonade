#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class ImagesVariationsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_variations";
        s.methods = {"POST"};
        s.paths = {"images/variations"};
        s.summary = "Image Variations";
        s.modality = "image -> varied image";
        s.description =
            "Image Variations API. You provide a source image and receive a variation of it.\n\n"
            "> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Unlike "
            "`/images/edits`, a `prompt` parameter is not supported and will be ignored — the "
            "model generates a variation based solely on the input image.\n"
            ">\n"
            "> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is "
            "significantly faster.";
        s.notes = {
            "### Create Variations with the OpenAI Python Client\n"
            "\n"
            "=== \"Python (OpenAI client)\"\n"
            "\n"
            "    ```python\n"
            "    from openai import OpenAI\n"
            "    client = OpenAI(base_url=\"http://localhost:13305/api/v1\", api_key=\"not-needed\")\n"
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
             "The Stable Diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`)."},
            {"image", ArgIn::Form, {{"type", "string"}, {"format", "binary"}}, true, Support::Available,
             "The source image file (PNG). Sent as a file in multipart/form-data."},
            {"size", ArgIn::Form, text, false, Support::Available,
             "The size of the output image. Format: `WIDTHxHEIGHT` (e.g., `512x512`). Default: "
             "`512x512`."},
            {"n", ArgIn::Form, {{"type", "integer"}}, false, Support::Partial,
             "Number of variations to generate. Integer between 1 and 10 inclusive. Default: `1`. "
             "Values outside this range result in a 400 Bad Request error."},
            {"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
             "Format of the response. Only `b64_json` (base64-encoded image) is supported."},
            {"user", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted but not forwarded to the backend."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = ordered_json::parse(R"({
            "model": "Flux-2-Klein-4B",
            "size": "512x512",
            "n": 1,
            "response_format": "b64_json",
            "image": "@fixtures/image.png"
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
