#include "alias_errors.h"

#include <nlohmann/json.hpp>

namespace lemon {

const RouteError kAliasStoreFailed{
    500, "internal_error",
    "The alias manager is not initialized, or reading or checking the alias file fails"};

void write_alias_error(httplib::Response& res, const RouteError& error, const std::string& message,
                       const std::string& type) {
    res.status = error.status;
    res.set_content(nlohmann::json{{"error", {
        {"message", message},
        {"type", type},
        {"param", "alias"},
        {"code", error.code}
    }}}.dump(), "application/json");
}

} // namespace lemon
