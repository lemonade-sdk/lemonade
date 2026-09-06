#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace lemon {

bool populate_image_request(const httplib::FormFiles& files,
                            nlohmann::json& request);

}  // namespace lemon
