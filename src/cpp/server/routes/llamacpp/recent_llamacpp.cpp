#include "recent_llamacpp.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/server/api_route.h"

namespace lemon {

const RouteError RecentLlamaCpp::kNoModelLoaded{400, "", "No model is loaded"};
// Router answers model_not_loaded and unsupported_operation, which set_error_response maps
// to 404 and 400.
const RouteError RecentLlamaCpp::kNotLlamaCpp{
    400, "", "The most recently used model is not a llama.cpp model"};

std::vector<RouteError> RecentLlamaCpp::errors() {
    return {kNoModelLoaded, kModelUnloaded, kNotLlamaCpp, kBackendError,
            ApiRoute::kUnexpectedException};
}

void RecentLlamaCpp::serve(Router& router, httplib::Response& res, const std::string& operation,
                           const std::function<nlohmann::json()>& call) {
    try {
        if (!router.is_model_loaded()) {
            LOG(ERROR, "Server") << "No model loaded for " << operation << std::endl;
            write_plain_error(res, kNoModelLoaded, "No model loaded for " + operation);
            return;
        }

        auto response = call();
        if (response.contains("error")) {
            set_error_response(response, res, kBackendError);
            return;
        }

        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "ERROR in " << operation << ": " << e.what() << std::endl;
        write_plain_error(res, ApiRoute::kUnexpectedException, e.what());
    }
}

} // namespace lemon
