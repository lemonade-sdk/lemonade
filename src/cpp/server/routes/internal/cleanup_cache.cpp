#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_loader.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

// The body is ModelLoader::model_error() for an empty model name, which carries model_not_found.
const RouteError kCleanupFailed{
    500, "model_not_found",
    "Scanning or removing cache files fails"};

const RouteArg kDryRun{"dry_run", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
                       "List the orphaned files without removing them. Defaults to `true`."};

class CleanupCacheRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.cleanup_cache";
        s.methods = {"POST"};
        s.paths = {"cleanup-cache"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Find or remove orphaned model files";
        s.description =
            "Finds, and optionally removes, model files that a multi-repository model left in "
            "another repository's Hugging Face cache folder.";
        s.notes = {
            "An orphan is a non-`main` checkpoint file, such as an `mmproj` from a different "
            "repository, found inside the `main` checkpoint's cache folder.",
        };
        s.args = {kDryRun};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["orphaned_files", "total_bytes", "dry_run"],
            "properties": {
                "orphaned_files": {"type": "array", "description": "Each orphaned file found.", "items": {
                    "type": "object",
                    "required": ["path", "size", "model", "type", "belongs_to"],
                    "properties": {
                        "path": {"type": "string", "description": "Absolute path of the file."},
                        "size": {"type": "integer", "description": "Size in bytes."},
                        "model": {"type": "string", "description": "Model whose checkpoint the file is."},
                        "type": {"type": "string", "description": "Checkpoint role, such as `mmproj`."},
                        "belongs_to": {"type": "string", "description": "Repository the file belongs to."}
                    }
                }},
                "total_bytes": {"type": "integer", "description": "Combined size of the orphaned files."},
                "dry_run": {"type": "boolean", "description": "Whether the files were left in place."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"dry_run", true}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgInvalid, kCleanupFailed};
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            bool dry_run = req.value(kDryRun, true);

            auto result = ctx_.model_manager->cleanup_orphaned_cache(dry_run);
            res.set_content(result.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_cleanup_cache: " << e.what() << std::endl;
            res.status = kCleanupFailed.status;
            res.set_content(ctx_.model_loader->model_error("", e.what()).dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_cleanup_cache_route(ServerContext& ctx) {
    return std::make_unique<CleanupCacheRoute>(ctx);
}

} // namespace lemon
