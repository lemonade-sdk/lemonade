#include "lemon/server/image_responses.h"

#include <algorithm>

#include <lemon/utils/aixlog.hpp>

#include "lemon/utils/json_utils.h"

namespace lemon {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

namespace {

const RouteError kImageInvalidJson{
    400, "",
    "The backend's reply does not parse, or a request field not listed above has the wrong JSON type"};
const RouteError kImageFieldsMixed{400, "", "The form has files under both `image` and `image[]`"};

} // namespace

const RouteError kImageBackendError{
    500, "",
    "The backend reports an error, for example when `model` cannot serve this route, an error "
    "whose `type` is `unsupported_operation`"};

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
    return {kImageInvalidJson, ApiRoute::kUnexpectedException};
}

void write_image_route_exception(const std::exception& error, httplib::Response& res,
                                 const char* fallback_type) {
    if (dynamic_cast<const json::exception*>(&error)) {
        LOG(ERROR, "Server") << "JSON parse error in image route: " << error.what() << std::endl;
        write_openai_error(res, kImageInvalidJson, "Invalid JSON: " + std::string(error.what()));
        return;
    }
    LOG(ERROR, "Server") << "ERROR in image route: " << error.what() << std::endl;
    write_openai_error(res, ApiRoute::kUnexpectedException, error.what(), fallback_type);
}

void write_image_backend_response(const json& response, httplib::Response& res) {
    if (response.contains("error")) {
        LOG(ERROR, "Server") << "Image backend error: " << response.dump() << std::endl;
        res.status = kImageBackendError.status;
    }
    res.set_content(response.dump(), "application/json");
}

std::vector<RouteError> attach_form_image_errors(bool allow_multiple) {
    if (allow_multiple) {
        return {kImageFieldsMixed};
    }
    return {};
}

bool attach_form_image(RouteRequest& req, httplib::Response& res, const RouteArg& image,
                       bool allow_multiple) {
    const auto& files = req.http.form.files;
    std::vector<std::string> spellings = {image.name};
    spellings.insert(spellings.end(), image.aliases.begin(), image.aliases.end());
    // A multipart map groups files by field name, losing mixed upload order.
    const auto spellings_sent = std::count_if(spellings.begin(), spellings.end(),
                                              [&files](const std::string& name) { return files.count(name) > 0; });
    if (allow_multiple && spellings_sent > 1) {
        write_openai_error(res, kImageFieldsMixed, "Use either 'image' or 'image[]', not both");
        return false;
    }

    json images = json::array();
    for (const auto& [field, file] : files) {
        if (std::find(spellings.begin(), spellings.end(), field) == spellings.end()) {
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
    req.body["image_files"] = std::move(images);
    return true;
}

} // namespace lemon
