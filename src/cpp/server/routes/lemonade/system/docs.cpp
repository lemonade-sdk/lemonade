#include <string>

#include "lemon/api_docs.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Lists the API reference pages bundled with the server, which describe the running "
            "version and need no internet access. Fetch this index first, then read the pages it "
            "lists.";

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["version", "format", "docs"],
            "properties": {
                "version": {"type": "string", "description": "Lemonade Server version the pages describe."},
                "format": {"const": "text/markdown", "description": "The media type of every listed page."},
                "docs": {"type": "array", "description": "The bundled pages.", "items": {
                    "type": "object",
                    "required": ["id", "title", "url", "bytes"],
                    "properties": {
                        "id": {"type": "string", "description": "Page id, mirroring the page's path on the documentation website."},
                        "title": {"type": "string", "description": "The page's title."},
                        "url": {"type": "string", "description": "Where to read the page. Clients follow it rather than building URLs, so a release can add pages without breaking them."},
                        "bytes": {"type": "integer", "description": "The page's size in bytes."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
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
