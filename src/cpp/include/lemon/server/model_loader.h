#pragma once

#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/model_residency.h"
#include "lemon/router.h"
#include "lemon/server/route_error.h"

namespace lemon {

// Inference routes and the gateways load a model on first use. Keeping that path,
// collection loading and the load-error response in one class means every route
// loads and fails the same way.
class ModelLoader {
public:
    ModelLoader(Router* router, ModelManager* model_manager);

    // load_options apply only to a first load, so options set by an explicit /v1/load win.
    // Pass load_options(), never a raw request body, so request-scoped fields stay
    // out of persistent recipe options.
    void ensure_loaded(const std::string& model,
                       const nlohmann::json& load_options = nlohmann::json::object(),
                       LoadPurpose purpose = LoadPurpose::UserInference);

    // A collection has no backend of its own, so loading one loads each component.
    void ensure_collection_loaded(const ModelInfo& info);

    // The load-level options for a request's ctx_size value, null when it sent none.
    static nlohmann::json load_options(const nlohmann::json& ctx_size);

    // An actionable error for a model that is filtered out on this system, not in the
    // registry, or failed to load.
    nlohmann::json model_error(const std::string& model, const std::string& message) const;

    void write_load_error(httplib::Response& res, const std::string& model,
                          const std::string& message) const;

    static const RouteError kModelNotSupported;
    static const RouteError kModelNotFound;
    static const RouteError kRouterResidencyConflict;
    static const RouteError kSlotsPinned;
    static const RouteError kModelLoadError;
    static std::vector<RouteError> errors();
    static const RouteError& load_error(const std::string& error_code);

private:
    Router* router_;
    ModelManager* model_manager_;
};

} // namespace lemon
