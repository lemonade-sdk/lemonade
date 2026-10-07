#include "lemon/alias_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class AliasesDeleteRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.aliases_delete";
        s.methods = {"DELETE"};
        s.paths = {"aliases/{alias}"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Remove a model alias";
        s.description = "Removes a model alias. Its target model is unaffected.";
        s.notes = {
            "An alias that does not exist answers `404` with code `alias_not_found`.",
        };
        s.args = {
            {"alias", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "Alias to remove."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status", "alias"],
            "properties": {
                "status": {"const": "deleted"},
                "alias": {"type": "string"}
            }
        })");
        response.setup = {{"internal.aliases_create", ResponseFormat::Json}};
        response.example = {{"alias", "$internal.aliases_create/alias"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string alias = utils::normalize_model_name(req.http.matches[1]);
        auto write_error = [&res](int status, const std::string& message, const std::string& code,
                                  const std::string& type) {
            res.status = status;
            res.set_content(json{{"error", {
                {"message", message},
                {"type", type},
                {"param", "alias"},
                {"code", code}
            }}}.dump(), "application/json");
        };
        try {
            if (!ctx_.alias_manager->remove_alias(alias)) {
                write_error(404, "Alias not found: " + alias, "alias_not_found",
                            "invalid_request_error");
                return;
            }
            json response = {
                {"status", "deleted"},
                {"alias", alias}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_error(500, e.what(), "internal_error", "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_delete_route(ServerContext& ctx) {
    return std::make_unique<AliasesDeleteRoute>(ctx);
}

} // namespace lemon
