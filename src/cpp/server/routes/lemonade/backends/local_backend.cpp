#include "local_backend.h"

#include "lemon/server/api_route.h"

namespace lemon {

const RouteError kMissingRecipe{
    400, "", "A local request has no `recipe`"};

bool read_local_backend(const nlohmann::json& body, std::string& recipe, std::string& backend,
                        httplib::Response& res) {
    recipe = body.value("recipe", "");
    backend = body.value("backend", "");
    if (recipe.empty()) {
        write_plain_error(res, kMissingRecipe, "A local backend requires 'recipe'");
        return false;
    }
    return true;
}

} // namespace lemon
