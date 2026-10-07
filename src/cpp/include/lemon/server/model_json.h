#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"

namespace lemon {

class AliasManager;
class Router;

// The models routes, the job engine and the status page all describe a model, so the
// shape is built here once and its schema sits beside it.
class ModelJson {
public:
    ModelJson(ModelManager* model_manager, Router* router, AliasManager* alias_manager);

    // The model object served by GET /v1/models and GET /v1/models/{id}. depth bounds how
    // deeply collection components embed one another, since registrations can be cyclic.
    nlohmann::json to_json(const std::string& model_id, const ModelInfo& info,
                           int depth = 0) const;

    // The smaller window.SERVER_MODELS entry the legacy status page reads.
    nlohmann::json status_page_json(const std::string& model_id, const ModelInfo& info) const;

    static nlohmann::json schema();

private:
    // Effective context window, or 0 when nothing knows it: a loaded backend's own ctx_size,
    // else the resolved options, else max_context_window. It skips the VRAM auto-tuner, which
    // probes the system on every call and would do so once per model across a listing.
    int64_t resolve_context_length(const std::string& model_id, const ModelInfo& info) const;

    ModelManager* model_manager_;
    Router* router_;
    AliasManager* alias_manager_;
};

} // namespace lemon
