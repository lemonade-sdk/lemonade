#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class ModelsIdFilesRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_files";
        s.methods = {"GET"};
        s.paths = {"models/{id}/files"};
        s.summary = "List resolved local file metadata for one model";
        s.description =
            "Lemonade extension: lists the files one model resolves to on disk, for model-detail "
            "UIs such as a Files tab. It is per-model inventory, not drive storage accounting.";
        s.notes = {
            "Absolute paths are left out by default, since they can reveal local usernames and "
            "the cache layout. Trusted local clients that need them for native UI actions can ask "
            "with `?include_paths=true`.",
            "An unknown model answers `404` with an `error` object whose `code` is "
            "`model_not_found`.",
        };
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "Model id, as listed by [`GET /v1/models`](#get-v1models)."},
            {"include_paths", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
             "`true` adds each file's absolute `path`. Defaults to `false`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["model_id", "files"],
            "properties": {
                "model_id": {"type": "string", "description": "Public model id of the requested model."},
                "files": {"type": "array", "description": "Resolved model files known to the registry.", "items": {
                    "type": "object",
                    "required": ["name", "role", "size_bytes", "exists"],
                    "properties": {
                        "name": {"type": "string", "description": "Base filename from the resolved path."},
                        "path": {"type": "string", "description": "Absolute resolved path on the local system. Only with include_paths=true; privacy-sensitive."},
                        "role": {"type": "string", "description": "Checkpoint role, for example main, mmproj, or another recipe-specific role."},
                        "size_bytes": {"type": "integer", "description": "File size in bytes. Directories are summed recursively; missing files report 0."},
                        "exists": {"type": "boolean", "description": "Whether the resolved path currently exists on disk."}
                    }
                }}
            }
        })");
        response.example = {{"id", "Qwen3-0.6B-GGUF"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager& model_manager = *ctx_.model_manager;
        std::string model_id = req.http.matches[1];
        const bool include_paths = req.http.has_param("include_paths") &&
            req.http.get_param_value("include_paths") == "true";

        try {
            if (!model_manager.model_exists(model_id)) {
                ctx_.model_loader->write_load_error(res, model_id, "Model not found");
                return;
            }

            std::string canonical_cache_key = model_manager.resolve_model_name(model_id);
            std::string wire_id = model_manager.get_public_model_name(canonical_cache_key);
            auto files = model_manager.list_model_files(model_id);

            json response;
            response["model_id"] = wire_id;
            response["files"] = json::array();

            for (const auto& file : files) {
                json file_json = {
                    {"name", file.name},
                    {"role", file.role},
                    {"size_bytes", file.size_bytes},
                    {"exists", file.exists}
                };

                if (include_paths) {
                    file_json["path"] = file.path;
                }

                response["files"].push_back(std::move(file_json));
            }

            res.set_content(response.dump(), "application/json");
        } catch (const std::exception&) {
            res.status = 404;
            res.set_content(ctx_.model_loader->model_error(model_id, "Model not found").dump(),
                            "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_files_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdFilesRoute>(ctx);
}

} // namespace lemon
