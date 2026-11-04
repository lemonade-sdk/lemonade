#include "local_backend.h"

#include "lemon/server/api_route.h"

namespace lemon {

const RouteError kMissingRecipe{
    400, "", "A local request has no `recipe`"};

bool require_local_recipe(const std::string& recipe, httplib::Response& res) {
    if (recipe.empty()) {
        write_plain_error(res, kMissingRecipe, "A local backend requires 'recipe'");
        return false;
    }
    return true;
}

} // namespace lemon
