#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/image_responses.h"
#include "lemon/utils/json_utils.h"

namespace lemon {

namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

std::optional<std::pair<int, int>> image_dimensions_from_bytes(const std::string& bytes) {
    auto at = [&bytes](size_t i) -> std::uint8_t {
        return static_cast<std::uint8_t>(bytes[i]);
    };
    if (bytes.size() >= 24 &&
        bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        bytes.compare(12, 4, "IHDR", 4) == 0) {
        auto be32 = [&](size_t i) -> std::uint32_t {
            return (std::uint32_t(at(i)) << 24) | (std::uint32_t(at(i + 1)) << 16) |
                   (std::uint32_t(at(i + 2)) << 8) | std::uint32_t(at(i + 3));
        };
        const auto width = be32(16);
        const auto height = be32(20);
        if (width > 0 && height > 0 &&
            width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
            height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return std::pair<int, int>{static_cast<int>(width), static_cast<int>(height)};
        }
    }
    if (bytes.size() >= 4 && at(0) == 0xff && at(1) == 0xd8) {
        size_t pos = 2;
        while (pos < bytes.size()) {
            if (at(pos) != 0xff) break;
            while (pos < bytes.size() && at(pos) == 0xff) ++pos;
            if (pos >= bytes.size()) break;
            const auto marker = at(pos++);
            if (marker == 0xd9 || marker == 0xda) break;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (bytes.size() - pos < 2) break;
            const size_t segment_len = (size_t(at(pos)) << 8) | at(pos + 1);
            if (segment_len < 2 || segment_len > bytes.size() - pos) break;
            const bool sof = marker >= 0xc0 && marker <= 0xcf &&
                             marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
            if (sof && segment_len >= 7) {
                const int height = (int(at(pos + 3)) << 8) | at(pos + 4);
                const int width = (int(at(pos + 5)) << 8) | at(pos + 6);
                if (width > 0 && height > 0) return std::pair<int, int>{width, height};
            }
            pos += segment_len;
        }
    }
    return std::nullopt;
}

constexpr std::uint64_t kDefaultMaxEditPixels = 2048ULL * 2048ULL;

std::pair<int, int> cap_image_area(int width, int height, std::uint64_t max_pixels) {
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels <= max_pixels) return {width, height};

    const double scale = std::sqrt(static_cast<double>(max_pixels) / pixels);
    int scaled_width = std::max(1, static_cast<int>(width * scale));
    int scaled_height = std::max(1, static_cast<int>(height * scale));
    // A dimension rounded up to one pixel must not violate the area cap.
    if (static_cast<std::uint64_t>(scaled_width) * scaled_height > max_pixels) {
        if (scaled_width > scaled_height) {
            scaled_width = static_cast<int>(max_pixels / scaled_height);
        } else {
            scaled_height = static_cast<int>(max_pixels / scaled_width);
        }
    }
    return {scaled_width, scaled_height};
}

class ImagesEditsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_edits";
        s.methods = {"POST"};
        s.paths = {"images/edits"};
        s.summary = "Image Editing";
        s.modality = "image + prompt -> edited image";
        s.description =
            "Image Editing API. Provide one or more reference images and a prompt describing the "
            "desired change, and receive an edited image.\n\n"
            "> **Note:** This endpoint accepts `multipart/form-data` requests (not JSON). Use "
            "editing-capable models such as `Flux-2-Klein-4B` or `SD-Turbo`.\n"
            ">\n"
            "> **Performance:** CPU inference takes several minutes per image. GPU (ROCm) is "
            "significantly faster.";
        s.notes = {
            "### Edit with the OpenAI Python Client\n"
            "\n"
            "=== \"Python (OpenAI client)\"\n"
            "\n"
            "    ```python\n"
            "    from openai import OpenAI\n"
            "    client = OpenAI(base_url=\"http://localhost:13305/api/v1\", api_key=\"not-needed\")\n"
            "    with open(\"source_image.png\", \"rb\") as image_file:\n"
            "        response = client.images.edit(\n"
            "            model=\"Flux-2-Klein-4B\",\n"
            "            image=image_file,\n"
            "            prompt=\"Add a red barn and mountains in the background, photorealistic\",\n"
            "            size=\"512x512\",\n"
            "        )\n"
            "    import base64\n"
            "    image_data = base64.b64decode(response.data[0].b64_json)\n"
            "    open(\"edited_image.png\", \"wb\").write(image_data)\n"
            "    ```",
        };
        const ordered_json integer = {{"type", "integer"}};
        const ordered_json text = {{"type", "string"}};
        const ordered_json file = {{"type", "string"}, {"format", "binary"}};
        s.args = {
            {"model", ArgIn::Form, text, true, Support::Available,
             "The diffusion model to use (e.g., `Flux-2-Klein-4B`, `SD-Turbo`)."},
            {"image", ArgIn::Form, file, true, Support::Available,
             "PNG or JPEG image upload. For multiple references, repeat `image[]` in order. Files "
             "are forwarded without re-encoding."},
            {"prompt", ArgIn::Form, text, true, Support::Available,
             "A text description of the desired edit."},
            {"mask", ArgIn::Form, file, false, Support::Available,
             "An optional mask image (PNG). White areas indicate regions to edit; black areas are "
             "preserved."},
            {"size", ArgIn::Form, text, false, Support::Available,
             "Output dimensions as `WIDTHxHEIGHT` (e.g., `512x512`). When omitted, use the first "
             "reference image's dimensions, scaling down proportionally to the model's optional "
             "`image_defaults.soft_pixel_cap` cap (default: 2048x2048 pixels, ~4 MP). An explicit "
             "size bypasses this soft cap; backend limits still apply."},
            {"n", ArgIn::Form, integer, false, Support::Partial,
             "Number of images to generate. Allowed range: `1`–`10`. Default: `1`. Values outside "
             "this range are rejected with `400 Bad Request`."},
            {"response_format", ArgIn::Form, {{"const", "b64_json"}}, false, Support::Partial,
             "Format of the response. Only `b64_json` (base64-encoded image) is supported."},
            {"steps", ArgIn::Form, integer, false, Support::Available,
             "Number of inference steps. Default varies by model."},
            {"cfg_scale", ArgIn::Form, {{"type", "number"}}, false, Support::Available,
             "Classifier-free guidance scale. Default varies by model."},
            {"seed", ArgIn::Form, integer, false, Support::Available, "Random seed for reproducibility."},
            {"user", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted but not forwarded to the backend."},
            {"background", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted but not forwarded to the backend."},
            {"quality", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted but not forwarded to the backend."},
            {"input_fidelity", ArgIn::Form, text, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted but not forwarded to the backend."},
            {"output_compression", ArgIn::Form, integer, false, Support::NotAvailable,
             "OpenAI API compatibility field. Accepted; silently ignored by the backend."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        response.example = ordered_json::parse(R"({
            "model": "Flux-2-Klein-4B",
            "prompt": "Add a red barn and mountains in the background, photorealistic",
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
        LOG(INFO, "Server") << "POST /api/v1/images/edits" << std::endl;
        if (!attach_form_image(req, res, /*allow_multiple=*/true)) {
            return false;
        }

        // The first uploaded image sets the output size unless explicitly provided.
        if (!req.body.contains("size")) {
            std::uint64_t soft_pixel_cap = kDefaultMaxEditPixels;
            const auto model = ctx_.router->try_get_model_info(req.model);
            if (model && model->image_defaults.soft_pixel_cap > 0) {
                soft_pixel_cap = model->image_defaults.soft_pixel_cap;
            }
            const httplib::FormData* first = find_form_file(req.http, {"image", "image[]"});
            const auto original = image_dimensions_from_bytes(first->content);
            if (original) {
                const auto [width, height] =
                    cap_image_area(original->first, original->second, soft_pixel_cap);
                req.body["size"] = std::to_string(width) + "x" + std::to_string(height);
            } else {
                LOG(WARNING, "Server") << "Cannot determine source image dimensions; "
                    "using backend defaults" << std::endl;
            }
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
