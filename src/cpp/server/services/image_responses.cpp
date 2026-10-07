#include "lemon/server/image_responses.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/utils/json_utils.h"

namespace lemon {

using json = nlohmann::json;

json image_response_schema() {
    return json::parse(R"({
        "type": "object",
        "required": ["created", "data"],
        "properties": {
            "created": {"type": "integer", "description": "Unix timestamp of when the image was generated."},
            "data": {"type": "array", "items": {
                "type": "object",
                "required": ["b64_json"],
                "properties": {
                    "b64_json": {"type": "string", "description": "Base64-encoded PNG."}
                }
            }}
        }
    })");
}

void write_image_route_exception(const std::exception& error, httplib::Response& res,
                                 const char* fallback_type) {
    if (dynamic_cast<const json::exception*>(&error)) {
        LOG(ERROR, "Server") << "JSON parse error in image route: " << error.what() << std::endl;
        write_openai_error(res, 400, "Invalid JSON: " + std::string(error.what()));
        return;
    }
    LOG(ERROR, "Server") << "ERROR in image route: " << error.what() << std::endl;
    write_openai_error(res, 500, error.what(), fallback_type);
}

bool attach_form_image(RouteRequest& req, httplib::Response& res) {
    if (req.body.contains("n")) {
        const int n = req.body["n"].get<int>();
        if (n < 1 || n > 10) {
            write_openai_error(res, 400, "Invalid value for 'n': must be between 1 and 10");
            return false;
        }
    }

    const httplib::FormData* file = find_form_file(req.http, {"image", "image[]"});
    if (!file) {
        write_openai_error(res, 400, "Missing 'image' field in request");
        return false;
    }
    req.body["image_data"] = utils::JsonUtils::base64_encode(file->content);
    req.body["image_filename"] = file->filename;
    LOG(INFO, "Server") << "Image file: " << file->filename
                        << " (" << file->content.size() << " bytes)" << std::endl;
    return true;
}

} // namespace lemon
