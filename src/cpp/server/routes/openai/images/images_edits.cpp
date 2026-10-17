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
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"
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

const ordered_json kIntegerSchema = {{"type", "integer"}};
const ordered_json kStringSchema = {{"type", "string"}};
const ordered_json kFileSchema = {{"type", "string"}, {"format", "binary"}};

const RouteArg kImage{"image", ArgIn::Form, kFileSchema, true, Support::Available,
                      "PNG or JPEG image upload. For multiple references, repeat `image[]` in order. Files "
                      "are forwarded without re-encoding.", {"image[]"}};
const RouteArg kPrompt{"prompt", ArgIn::Form, kStringSchema, true, Support::Available,
                       "A text description of the desired edit."};
const RouteArg kMask{"mask", ArgIn::Form, kFileSchema, false, Support::Available,
                     "A PNG mask: white areas are edited and black areas are kept."};
const RouteArg kSize{"size", ArgIn::Form, kStringSchema, false, Support::Available,
                     "Output size as `WIDTHxHEIGHT`, such as `512x512`. Defaults to the first reference "
                     "image's size, scaled down to fit the model's `image_defaults.soft_pixel_cap` "
                     "(2048x2048 pixels, about 4 MP, unless the model sets one). An explicit size skips "
                     "that cap; backend limits still apply."};
const RouteArg kSteps{"steps", ArgIn::Form, kIntegerSchema, false, Support::Available,
                      "Number of inference steps. Defaults to the model's own."};
const RouteArg kCfgScale{"cfg_scale", ArgIn::Form, {{"type", "number"}}, false, Support::Available,
                         "Classifier-free guidance scale. Defaults to the model's own."};
const RouteArg kSeed{"seed", ArgIn::Form, kIntegerSchema, false, Support::Available, "Random seed for reproducible output."};
const RouteArg kBackground{"background", ArgIn::Form, kStringSchema, false, Support::NotAvailable,
                           "Accepted for OpenAI compatibility and ignored."};
const RouteArg kQuality{"quality", ArgIn::Form, kStringSchema, false, Support::NotAvailable,
                        "Accepted for OpenAI compatibility and ignored."};
const RouteArg kInputFidelity{"input_fidelity", ArgIn::Form, kStringSchema, false, Support::NotAvailable,
                              "Accepted for OpenAI compatibility and ignored."};
const RouteArg kOutputCompression{"output_compression", ArgIn::Form, kIntegerSchema, false, Support::NotAvailable,
                                  "Accepted for OpenAI compatibility and ignored."};

class ImagesEditsRoute : public ModelRoute {
public:
    using ModelRoute::ModelRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.images_edits";
        s.methods = {"POST"};
        s.paths = {"images/edits"};
        s.summary = "Edit images as a prompt describes";
        s.modality = "image + prompt -> edited image";
        s.description =
            "Edits one or more reference images as a prompt describes. The request is "
            "`multipart/form-data`. CPU inference takes minutes per image; a GPU backend is much faster.";
        s.notes = {
            "### Edit with the OpenAI Python Client\n"
            "\n"
            "=== \"Python (OpenAI client)\"\n"
            "\n"
            "    ```python\n"
            "    from openai import OpenAI\n"
            "    client = OpenAI(base_url=\"http://localhost:13305/v1\", api_key=\"not-needed\")\n"
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
        s.args = {
            model_loading::kFormModel, kImage, kPrompt, kMask, kSize, openai::image_uploads::kN,
            openai::image_uploads::kResponseFormat, kSteps, kCfgScale, kSeed,
            openai::image_uploads::kUser, kBackground, kQuality, kInputFidelity, kOutputCompression
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = image_response_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "model": "Flux-2-Klein-4B",
            "prompt": "Add a red barn and mountains in the background, photorealistic",
            "size": "512x512",
            "n": 1,
            "response_format": "b64_json",
            "image": "@fixtures/image.png"
        })");
        s.examples = {example};
        s.errors = join_errors({{kFormNotMultipart, kArgMissing, kArgInvalid,
                                 kImageBackendError},
                                attach_form_image_errors(/*allow_multiple=*/true),
                                image_route_exception_errors(), ModelLoader::errors()});
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Form;
        return s;
    }

protected:
    const RouteArg& model_arg() const override { return model_loading::kFormModel; }

    bool validate(RouteRequest& req, httplib::Response& res) override {
        LOG(INFO, "Server") << "POST /api/v1/images/edits" << std::endl;
        if (!attach_form_image(req, res, kImage, /*allow_multiple=*/true)) {
            return false;
        }

        // The first uploaded image sets the output size unless explicitly provided.
        if (!req.has(kSize)) {
            std::uint64_t soft_pixel_cap = kDefaultMaxEditPixels;
            const auto model = ctx_.router->try_get_model_info(req.model);
            if (model && model->image_defaults.soft_pixel_cap > 0) {
                soft_pixel_cap = model->image_defaults.soft_pixel_cap;
            }
            const httplib::FormData* first = req.file(kImage);
            const auto original = image_dimensions_from_bytes(first->content);
            if (original) {
                const auto [width, height] =
                    cap_image_area(original->first, original->second, soft_pixel_cap);
                req.body[kSize.name] = std::to_string(width) + "x" + std::to_string(height);
            } else {
                LOG(WARNING, "Server") << "Cannot determine source image dimensions; "
                    "using backend defaults" << std::endl;
            }
        }

        if (const httplib::FormData* mask = req.file(kMask)) {
            req.body["mask_data"] = utils::JsonUtils::base64_encode(mask->content);
            req.body["mask_filename"] = mask->filename;
            LOG(INFO, "Server") << "Mask file: " << mask->filename
                                << " (" << mask->content.size() << " bytes)" << std::endl;
        }

        return true;
    }

    void run(RouteRequest& req, httplib::Response& res) override {
        auto response = ctx_.router->image_edits(req.body);
        write_image_backend_response(response, res);
    }

    void write_exception(const std::exception& error, httplib::Response& res) const override {
        write_image_route_exception(error, res, "server_error");
    }};

} // namespace

std::unique_ptr<ApiRoute> make_images_edits_route(ServerContext& ctx) {
    return std::make_unique<ImagesEditsRoute>(ctx);
}

} // namespace lemon
