#include "lemon/server/omni_collection.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/collection_orchestrator.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/model_loader.h"

namespace lemon {

using json = nlohmann::json;

bool is_omni_collection(ServerContext& ctx, const std::string& model) {
    try {
        return !model.empty() && ctx.model_manager->model_exists(model) &&
               is_omni_collection_recipe(ctx.model_manager->get_model_info(model).recipe);
    } catch (const std::exception& e) {
        LOG(DEBUG, "Server") << "Collection check failed for '" << model << "': " << e.what()
                             << std::endl;
        return false;
    }
}

void run_omni_collection(ServerContext& ctx, RouteRequest& req, httplib::Response& res) {
    const ModelInfo info = ctx.model_manager->get_model_info(req.model);

    // Load the whole collection up front so the model is fully ready, not just its chat
    // component.
    try {
        ctx.model_loader->ensure_collection_loaded(info);
    } catch (const std::exception& e) {
        LOG(ERROR, "Server") << "Failed to load collection '" << info.model_name
                             << "': " << e.what() << std::endl;
        ctx.model_loader->write_load_error(res, info.model_name, e.what());
        return;
    }

    ModelLoader* loader = ctx.model_loader;
    Router* router = ctx.router;
    ModelManager* model_manager = ctx.model_manager;
    auto ensure_loaded = [loader](const std::string& model) { loader->ensure_loaded(model); };

    const bool is_streaming = req.body.contains("stream") && req.body["stream"].is_boolean() &&
                              req.body["stream"].get<bool>();
    if (is_streaming) {
        LOG(INFO, "Server") << "POST /api/v1/chat/completions - Collection (streaming): "
                            << info.model_name << std::endl;
        ApiRoute::stream_response(
            req, res,
            [request = req.body, info, router, model_manager, ensure_loaded](
                const std::string&, httplib::DataSink& sink) {
                CollectionOrchestrator orchestrator(*router, *model_manager, ensure_loaded);
                try {
                    orchestrator.chat_completion_stream(request, info, sink);
                } catch (const std::exception& e) {
                    LOG(ERROR, "Server") << "Collection streaming failed: " << e.what() << std::endl;
                }
            });
        return;
    }

    LOG(INFO, "Server") << "POST /api/v1/chat/completions - Collection: "
                        << info.model_name << std::endl;
    CollectionOrchestrator orchestrator(*router, *model_manager, ensure_loaded);
    json response = orchestrator.chat_completion(req.body, info);
    if (response.contains("error")) {
        set_error_response(response, res);
        return;
    }
    res.set_content(response.dump(), "application/json");
}

} // namespace lemon
