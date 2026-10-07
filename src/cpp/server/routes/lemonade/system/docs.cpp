#include <string>

#include "lemon/api_docs.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class DocsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.docs";
        s.methods = {"GET"};
        s.paths = {"docs"};
        s.summary = "List the API reference pages bundled with the running server";
        s.description =
            "Lists the API reference pages bundled with the server. The pages ship with the "
            "server, so they describe the version actually running and need no internet access.";
        s.notes = {
            "Fetch this index first, then read the pages it advertises with "
            "[`GET /v1/docs/{page}`](#get-v1docspage). Every entry carries its own `url`, so new "
            "pages can appear in future releases without breaking clients.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["version", "format", "docs"],
            "properties": {
                "version": {"type": "string", "description": "Lemonade Server version the pages describe."},
                "format": {"const": "text/markdown"},
                "docs": {"type": "array", "items": {
                    "type": "object",
                    "required": ["id", "title", "url", "bytes"],
                    "properties": {
                        "id": {"type": "string", "description": "Page id, mirroring the page's path on the documentation website."},
                        "title": {"type": "string"},
                        "url": {"type": "string", "description": "Where to read the page, under the same prefix the index was requested with: a client that queries /api/v0/docs receives /api/v0/docs/... URLs."},
                        "bytes": {"type": "integer"}
                    }
                }}
            }
        })");
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string docs_dir = utils::get_resource_path("resources/docs");

        // Echo back the prefix the client used so the URLs stay valid on all four.
        const std::string& path = req.http.path;
        std::string prefix = path.substr(0, path.size() - std::string("/docs").size());

        json docs = json::array();
        for (const ApiDoc& doc : list_api_docs(docs_dir)) {
            docs.push_back({
                {"id", doc.id},
                {"title", doc.title},
                {"url", prefix + "/docs/" + doc.id},
                {"bytes", doc.bytes}
            });
        }

        json body = {
            {"version", LEMON_VERSION_STRING},
            {"format", "text/markdown"},
            {"docs", docs}
        };
        res.set_content(body.dump(2), "application/json");
        res.status = 200;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_docs_route(ServerContext& ctx) {
    return std::make_unique<DocsRoute>(ctx);
}

} // namespace lemon
