#include "image_multipart.h"

#include "lemon/utils/json_utils.h"

#include <string>
#include <utility>

namespace lemon {

bool populate_image_request(const httplib::FormFiles& files,
                            nlohmann::json& request) {
    bool has_primary = false;
    nlohmann::json references = nlohmann::json::array();

    for (const auto& file_pair : files) {
        const std::string& field_name = file_pair.first;
        const auto& file = file_pair.second;
        const bool is_primary_field =
            field_name == "image" || field_name == "image[]";

        if (!has_primary && is_primary_field) {
            request["image_data"] =
                utils::JsonUtils::base64_encode(file.content);
            request["image_filename"] = file.filename;
            has_primary = true;
            continue;
        }

        if (field_name.compare(0, 5, "image") == 0) {
            references.push_back(
                utils::JsonUtils::base64_encode(file.content));
        }
    }

    if (!references.empty()) {
        request["references"] = std::move(references);
    }
    return has_primary;
}

}  // namespace lemon
