#include "lemon/server/vision_inputs.h"

#include <utility>

#include "lemon/router.h"
#include "lemon/server/shared_args.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

namespace lemon {

using json = nlohmann::json;

const RouteError kVisionImageInvalid{
    400, "", "An `image` is not a PNG, JPEG, BMP or WebP image, is a remote URL, or there are more "
             "than 16"};

namespace {

constexpr size_t kMaxImages = 16;

bool is_decodable_image(const std::string& head) {
    const auto sniffed = utils::sniff_image(head);
    // GIF is left out: OpenCV older than 4.11 cannot decode it.
    if (sniffed.ok()) return sniffed.mime != "image/gif";
    return head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0;
}

// The bare base64 payload of one image input, or "" with `error` set.
std::string image_payload(const json& item, std::string& error) {
    std::string value;
    if (item.is_string()) {
        value = item.get<std::string>();
    } else if (item.is_object() && item.contains("image_url")) {
        const json& url = item["image_url"];
        if (url.is_string()) {
            value = url.get<std::string>();
        } else if (url.is_object() && url.contains("url") && url["url"].is_string()) {
            value = url["url"].get<std::string>();
        }
    }
    if (value.empty()) {
        error = "must be a base64 string, a data URL, or {\"image_url\": ...}";
        return "";
    }
    // Never fetched, so the server makes no outbound request on a client's behalf.
    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
        error = "is a remote URL; send the image as base64 or a data URL";
        return "";
    }
    if (value.rfind("data:", 0) == 0) {
        const size_t comma = value.find(',');
        if (comma == std::string::npos || value.substr(0, comma).find(";base64") == std::string::npos) {
            error = "is a data URL without a base64 payload";
            return "";
        }
        value = value.substr(comma + 1);
    }
    if (!is_decodable_image(utils::JsonUtils::base64_decode(value.substr(0, 16)))) {
        error = "is not a PNG, JPEG, BMP or WebP image";
        return "";
    }
    return value;
}

} // namespace

bool normalize_vision_images(RouteRequest& req, httplib::Response& res) {
    const RouteArg& image = openai::vision::kImage;
    const json& raw = req.arg(image);
    const json inputs = raw.is_array() ? raw : json::array({raw});
    if (inputs.empty() || inputs.size() > kMaxImages) {
        write_openai_error(res, kVisionImageInvalid,
                           "'image' must hold 1 to " + std::to_string(kMaxImages) + " images");
        return false;
    }
    json images = json::array();
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string error;
        std::string payload = image_payload(inputs[i], error);
        if (payload.empty()) {
            write_openai_error(res, kVisionImageInvalid, "image[" + std::to_string(i) + "] " + error);
            return false;
        }
        images.push_back(std::move(payload));
    }
    req.body[image.name] = std::move(images);
    return true;
}

bool resolve_sole_vision_model(ServerContext& ctx, RouteRequest& req, httplib::Response& res,
                               ModelType type, const RouteError& no_sole_model) {
    // The router requires a model on every request, so an omitted one is resolved
    // here, which is only possible when exactly one model of the type is loaded.
    req.model = ctx.router->get_sole_loaded_model_of_type(type);
    if (req.model.empty()) {
        write_openai_error(res, no_sole_model,
                           "No 'model' specified and no single " + model_type_to_string(type) +
                               " model is loaded (load one, or name it in the request)");
        return false;
    }
    req.model_already_loaded = true;
    return true;
}

} // namespace lemon
