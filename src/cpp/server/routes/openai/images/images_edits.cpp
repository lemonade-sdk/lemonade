#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"
#include "lemon/utils/json_utils.h"

namespace lemon {

namespace {

using json = nlohmann::json;

class ImagesEditsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_edits";
        s.methods = {"POST"};
        s.paths = {"images/edits"};
        s.summary = "Image Editing";
        s.description =
            "Edits a source image as a text prompt describes, loading the model on first use. The "
            "request is `multipart/form-data`.";
        s.notes = {
            "Use an editing-capable model such as `Flux-2-Klein-4B` or `SD-Turbo`.",
            "**Performance:** CPU inference takes several minutes per image. GPU (ROCm) is "
            "significantly faster.",
        };
        const json integer = {{"type", "integer"}};
        const json text = {{"type", "string"}};
        const json file = {{"type", "string"}, {"format", "binary"}};
        s.args = {
            {"model", ArgIn::Form, text, true, Support::Available,
             "Diffusion model, e.g. `Flux-2-Klein-4B` or `SD-Turbo`."},
            {"image", ArgIn::Form, file, true, Support::Available,
             "Source image to edit (PNG). The field may also be named `image[]`."},
            {"prompt", ArgIn::Form, text, true, Support::Available, "Text description of the desired edit."},
            {"mask", ArgIn::Form, file, false, Support::Available,
             "Mask image (PNG). White areas are edited; black areas are preserved."},
            {"size", ArgIn::Form, text, false, Support::Available,
             "Output size as `WIDTHxHEIGHT`, e.g. `512x512`. Defaults to `512x512`."},
            {"n", ArgIn::Form, integer, false, Support::Partial,
             "Number of images to generate, from `1` to `10`. Defaults to `1`; values outside "
             "the range answer `400`."},
            {"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
             "Only `b64_json` (a base64-encoded image) is supported."},
            {"steps", ArgIn::Form, integer, false, Support::Available,
             "Number of inference steps. The default varies by model."},
            {"cfg_scale", ArgIn::Form, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance scale. The default varies by model."},
            {"seed", ArgIn::Form, integer, false, Support::Available, "Random seed for reproducibility."},
            {"user", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted but not forwarded to the backend."},
            {"background", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted but not forwarded to the backend."},
            {"quality", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted but not forwarded to the backend."},
            {"input_fidelity", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted but not forwarded to the backend."},
            {"output_compression", ArgIn::Form, integer, false, Support::NotAvailable,
             "OpenAI compatibility field. Accepted and ignored by the backend."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = json::parse(R"({
            "model": "SD-Turbo",
            "image": "@fixtures/image.png",
            "prompt": "Add a red barn in the background, photorealistic",
            "size": "256x256",
            "steps": 4
        })");
        s.responses = {response};
        s.request_format = RequestFormat::Form;
        return s;
    }

protected:
    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/edits" << std::endl;
        if (!attach_form_image(req, res)) {
            return false;
        }

        if (const httplib::FormData* mask = find_form_file(req.http, {"mask"})) {
            req.body["mask_data"] = utils::JsonUtils::base64_encode(mask->content);
            req.body["mask_filename"] = mask->filename;
            LOG(INFO, "Server") << "Mask file: " << mask->filename
                                << " (" << mask->content.size() << " bytes)" << std::endl;
        }

        if (!req.body.contains("prompt")) {
            write_openai_error(res, 400, "Missing 'prompt' field in request");
            return false;
        }
        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_edits(req.body);
        if (response.contains("error")) {
            LOG(ERROR, "Server") << "Image edits backend error: " << response.dump() << std::endl;
            res.status = 500;
        }
        res.set_content(response.dump(), "application/json");
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "server_error");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_images_edits_route(ServerContext& ctx) {
    return std::make_unique<ImagesEditsRoute>(ctx);
}

} // namespace lemon
