#pragma once

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/model_residency.h"
#include "lemon/router.h"

namespace lemon {

// Inference routes and the gateways load a model on first use. Keeping that path,
// collection loading and the load-error response in one class means every route
// loads and fails the same way.
class ModelLoader {
public:
    ModelLoader(Router* router, ModelManager* model_manager);

    // load_options apply only to a first load, so options set by an explicit /v1/load win.
    // Pass load_options(request), never a raw request body, so request-scoped fields stay
    // out of persistent recipe options.
    void ensure_loaded(const std::string& model,
                       const nlohmann::json& load_options = nlohmann::json::object(),
                       LoadPurpose purpose = LoadPurpose::UserInference);

    // A collection has no backend of its own, so loading one loads each component.
    void ensure_collection_loaded(const ModelInfo& info);

    // The load-level options an inference request may carry: only ctx_size.
    static nlohmann::json load_options(const nlohmann::json& request);

    // An actionable error for a model that is filtered out on this system, not in the
    // registry, or failed to load.
    nlohmann::json model_error(const std::string& model, const std::string& message) const;

    void write_load_error(httplib::Response& res, const std::string& model,
                          const std::string& message) const;

    static int load_error_status(const std::string& error_code);

private:
    Router* router_;
    ModelManager* model_manager_;
};

} // namespace lemon
