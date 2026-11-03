#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon {

class ModelManager;
class Router;
struct ModelInfo;

// The models routes, the job engine and the status page all describe a model, so the
// shape is built here once and its schema sits beside it.
class ModelJson {
public:
    ModelJson(ModelManager* model_manager, Router* router);

    // depth bounds how deeply collection components embed one another, since
    // registrations can be cyclic. `include_paths` is the opt-in to absolute
    // filesystem paths, as in /models/{id}/files.
    nlohmann::json to_json(const std::string& model_id, const ModelInfo& info,
                           int depth = 0, bool include_paths = false) const;

    // The smaller window.SERVER_MODELS entry the legacy status page reads.
    nlohmann::json status_page_json(const std::string& model_id, const ModelInfo& info) const;

    static nlohmann::ordered_json schema();

private:
    // Effective context window, or 0 when nothing knows it: a loaded backend's own ctx_size,
    // else the resolved options, else max_context_window. It skips the VRAM auto-tuner, which
    // probes the system on every call and would do so once per model across a listing.
    int64_t resolve_context_length(const std::string& model_id, const ModelInfo& info) const;

    ModelManager* model_manager_;
    Router* router_;
};

} // namespace lemon
