#include "lemon/model_resolution.h"

#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"

namespace lemon {

std::string resolve_model_with_default(const std::string& requested,
                                       ModelManager* model_manager) {
    if (model_manager && model_manager->model_exists(requested)) {
        return requested;
    }

    if (auto* cfg = RuntimeConfig::global()) {
        const std::string fallback = cfg->default_model();
        if (!fallback.empty() && model_manager && model_manager->model_exists(fallback)) {
            LOG(INFO, "ModelResolution")
                << "Model '" << requested << "' not found, using default model '"
                << fallback << "'" << std::endl;
            return fallback;
        }
    }

    return requested;
}

} // namespace lemon
