#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kFilesUnreadable{404, "model_load_error", "Resolving or listing `id`'s files fails"};

const RouteArg kId{"id", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
                   "Model id, as listed by [`GET /v1/models`](#get-v1models). An alias's id is not "
                   "accepted here; use its target."};
const RouteArg kIncludePaths{"include_paths", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
                             "`true` adds each file's absolute `path`, which can reveal local usernames and the "
                             "cache layout. Defaults to `false`."};

class ModelsIdFilesRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_files";
        s.methods = {"GET"};
        s.paths = {"models/{id}/files"};
        s.summary = "List one model's local files";
        s.modality = "n/a";
        s.description =
            "Lists the resolved local files of one model, for model-detail views such as the "
            "app's Files tab. It inventories one model, not drive storage.";
        s.args = {kId, kIncludePaths};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["model_id", "files"],
            "properties": {
                "model_id": {"type": "string", "description": "The model's public id."},
                "files": {"type": "array", "description": "The model's resolved files.", "items": {
                    "type": "object",
                    "required": ["name", "role", "size_bytes", "exists"],
                    "properties": {
                        "name": {"type": "string", "description": "Base filename from the resolved path."},
                        "path": {"type": "string", "description": "Absolute path; present only with `include_paths=true`."},
                        "role": {"type": "string", "description": "Checkpoint role, for example `main`, `mmproj`, or another recipe-specific role."},
                        "size_bytes": {"type": "integer", "description": "File size in bytes. Directories are summed recursively. Missing files report `0`."},
                        "exists": {"type": "boolean", "description": "Whether the resolved path currently exists on disk."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "Qwen3-0.6B-GGUF"}};
        s.examples = {example};
        s.errors = {ModelLoader::kModelNotSupported, ModelLoader::kModelNotFound, kFilesUnreadable};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager& model_manager = *ctx_.model_manager;
        std::string model_id = req.value(kId, std::string());
        const bool include_paths = req.value(kIncludePaths, false);

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
            res.status = kFilesUnreadable.status;
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
