#include "lemon/server/image_responses.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/utils/json_utils.h"

namespace lemon {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {

const RouteError kImageInvalidJson{
    400, "",
    "A JSON value in the request or in the backend's reply does not parse or has the wrong type"};
const RouteError kImageRouteFailure{500, "", "The request fails unexpectedly"};
const RouteError kImageCountOutOfRange{400, "", "`n` is outside `1` to `10`"};
const RouteError kImageFieldsMixed{400, "", "The form has files under both `image` and `image[]`"};
const RouteError kMissingImage{400, "", "The form has no file under `image` or `image[]`"};

} // namespace

ordered_json image_response_schema() {
    return ordered_json::parse(R"({
        "type": "object",
        "required": ["created", "data"],
        "properties": {
            "created": {"type": "integer", "description": "Unix timestamp of when the image was generated."},
            "data": {"type": "array", "description": "The generated images, one entry per image.", "items": {
                "type": "object",
                "required": ["b64_json"],
                "properties": {
                    "b64_json": {"type": "string", "description": "Base64-encoded PNG."}
                }
            }},
            "output_format": {"type": "string", "description": "File format of the images, such as `png`, when the backend reports it."}
        }
    })");
}

std::vector<RouteError> image_route_exception_errors() {
    return {kImageInvalidJson, kImageRouteFailure};
}

void write_image_route_exception(const std::exception& error, httplib::Response& res,
                                 const char* fallback_type) {
    if (dynamic_cast<const json::exception*>(&error)) {
        LOG(ERROR, "Server") << "JSON parse error in image route: " << error.what() << std::endl;
        write_openai_error(res, kImageInvalidJson, "Invalid JSON: " + std::string(error.what()));
        return;
    }
    LOG(ERROR, "Server") << "ERROR in image route: " << error.what() << std::endl;
    write_openai_error(res, kImageRouteFailure, error.what(), fallback_type);
}

std::vector<RouteError> attach_form_image_errors(bool allow_multiple) {
    if (allow_multiple) {
        return {kImageCountOutOfRange, kImageFieldsMixed, kMissingImage};
    }
    return {kImageCountOutOfRange, kMissingImage};
}

bool attach_form_image(RouteRequest& req, httplib::Response& res, bool allow_multiple) {
    if (req.body.contains("n")) {
        const int n = req.body["n"].get<int>();
        if (n < 1 || n > 10) {
            write_openai_error(res, kImageCountOutOfRange, "Invalid value for 'n': must be between 1 and 10");
            return false;
        }
    }

    const auto& files = req.http.form.files;
    // A multipart map groups files by field name, losing mixed upload order.
    if (allow_multiple && files.count("image") > 0 && files.count("image[]") > 0) {
        write_openai_error(res, kImageFieldsMixed, "Use either 'image' or 'image[]', not both");
        return false;
    }

    json images = json::array();
    for (const auto& [field, file] : files) {
        if (field != "image" && field != "image[]") {
            continue;
        }
        const std::string encoded = utils::JsonUtils::base64_encode(file.content);
        if (images.empty()) {
            req.body["image_data"] = encoded;
            req.body["image_filename"] = file.filename;
            req.body["image_content_type"] = file.content_type;
        }
        LOG(INFO, "Server") << "Image file: " << file.filename
                            << " (" << file.content.size() << " bytes)" << std::endl;
        if (!allow_multiple) {
            return true;
        }
        images.push_back({
            {"data", encoded},
            {"filename", file.filename},
            {"content_type", file.content_type},
        });
    }
    if (images.empty()) {
        write_openai_error(res, kMissingImage, "Missing 'image' field in request");
        return false;
    }
    req.body["image_files"] = std::move(images);
    return true;
}

} // namespace lemon
