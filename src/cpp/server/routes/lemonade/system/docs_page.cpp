#include <string>

#include "lemon/api_docs.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/path_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

const RouteError kUnknownPage{404, "", "`page` names no bundled page"};

const RouteArg kPage{"page", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
                     "The `id` from the [`GET /v1/docs`](#get-v1docs) index, which mirrors the page's path "
                     "on the documentation website. The `.md` suffix is optional."};

class DocsPageRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.docs_page";
        s.methods = {"GET"};
        s.paths = {"docs/{page}"};
        s.summary = "Read one bundled API reference page";
        s.description =
            "Reads one page from the [`GET /v1/docs`](#get-v1docs) index, as `text/markdown`.";
        s.notes = {
            "### Reading the Files Directly\n"
            "\n"
            "The same files are installed on disk, so they can be read without a running server:\n"
            "\n"
            "| Platform | Path |\n"
            "|----------|------|\n"
            "| Windows (per-user) | `%LOCALAPPDATA%\\lemonade_server\\bin\\resources\\docs\\` |\n"
            "| Windows (all users) | `C:\\Program Files\\Lemonade Server\\bin\\resources\\docs\\` |\n"
            "| macOS | `/Library/Application Support/Lemonade/resources/docs/` |\n"
            "| Linux (local) | `/usr/local/share/lemonade-server/resources/docs/` |\n"
            "| Linux (system) | `/usr/share/lemonade-server/resources/docs/` |\n"
            "| Linux (optional prefix) | `/opt/share/lemonade-server/resources/docs/` |\n"
            "| Linux (per-user) | `~/.local/share/lemonade-server/resources/docs/` |",
        };
        s.args = {kPage};

        RouteResponse response;
        response.format = ResponseFormat::Text;
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Text;
        example.args = {{"page", "api/lemonade"}};
        s.examples = {example};
        s.errors = {kUnknownPage};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string docs_dir = utils::get_resource_path("resources/docs");

        std::string content;
        if (!read_api_doc(docs_dir, req.value(kPage, std::string()), content)) {
            write_plain_error(res, kUnknownPage, "Documentation page not found");
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
