#include "local_backend.h"

#include "lemon/backends/backend_registry.h"
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

const RouteError kInactiveBackend{
    400, "", "`recipe` and `backend` name a staged backend that is not active in this build"};

bool require_active_backend(const std::string& recipe, const std::string& backend, httplib::Response& res) {
    if (!backends::is_inactive_backend_selector(recipe, backend)) {
        return true;
    }
    write_plain_error(res, kInactiveBackend,
                      "Backend " + recipe + ":" + backend + " is staged and not active in this build");
    return false;
}

} // namespace lemon
