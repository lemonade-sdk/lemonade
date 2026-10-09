#include "lemon/server/cloud_providers.h"

#include <sstream>

#include <lemon/utils/aixlog.hpp>

#include "lemon/cloud_provider_registry.h"
#include "lemon/config_file.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"

namespace lemon {

using json = nlohmann::json;

void persist_cloud_providers(ServerContext& ctx) {
    const std::string& config_dir = ctx.server->config_dir();
    if (config_dir.empty()) return;
    try {
        json user_cfg = ConfigFile::load_raw(config_dir);
        user_cfg["cloud_providers"] = ctx.cloud_registry->to_config_array();
        ConfigFile::save(config_dir, user_cfg);
    } catch (const std::exception& e) {
        LOG(WARNING, "Server") << "Failed to persist cloud_providers to config.json: "
                               << e.what() << std::endl;
    }
}

void write_insecure_http_error(httplib::Response& res, const std::string& provider) {
    write_openai_error(res, 400,
                       "Cloud provider '" + provider + "' uses http://. "
                       "Set allow_insecure_http=true to explicitly opt in before "
                       "storing or using an API key over plaintext HTTP.",
                       "invalid_request_error", "insecure_http_requires_opt_in");
}

void attach_warnings(json& response, const std::vector<std::string>& warnings) {
    if (warnings.empty()) {
        return;
    }
    response["warnings"] = warnings;
    std::ostringstream joined;
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i > 0) {
            joined << " | ";
        }
        joined << warnings[i];
    }
    response["warning"] = joined.str();
}

} // namespace lemon
