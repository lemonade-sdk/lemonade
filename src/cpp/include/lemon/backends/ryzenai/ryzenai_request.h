#pragma once

#include <nlohmann/json.hpp>

namespace lemon {
namespace ryzenai {

nlohmann::json normalize_chat_request(const nlohmann::json& request);

} // namespace ryzenai
} // namespace lemon
