#include <string>

#include "lemon/api_docs.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/path_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class DocsPageRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.docs_page";
        s.methods = {"GET"};
        s.paths = {"docs/{page}"};
        s.summary = "Read one bundled API reference page";
        s.description = "Returns one bundled page as Markdown (`Content-Type: text/markdown`).";
        s.notes = {
            "Unknown pages return `404`.",
        };
        s.args = {
            {"page", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "The `id` from the [`GET /v1/docs`](#get-v1docs) index, which mirrors the page's path "
             "on the documentation website. The `.md` suffix is optional."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Text;
        response.example = {{"page", "api/README"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string docs_dir = utils::get_resource_path("resources/docs");

        std::string content;
        if (!read_api_doc(docs_dir, req.http.matches[1].str(), content)) {
            write_plain_error(res, 404, "Documentation page not found");
            return;
        }

        res.set_content(content, "text/markdown");
        res.status = 200;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_docs_page_route(ServerContext& ctx) {
    return std::make_unique<DocsPageRoute>(ctx);
}

} // namespace lemon
