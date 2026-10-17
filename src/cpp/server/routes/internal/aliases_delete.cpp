#include "alias_errors.h"
#include "lemon/alias_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kAliasNotFound{404, "alias_not_found", "`alias` names no alias"};

const RouteArg kAlias{"alias", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
                      "The alias name to remove."};

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
        s.description = "Removes a model alias.";
        s.args = {kAlias};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "alias"],
            "properties": {
                "status": {"const": "deleted", "description": "Always `deleted`."},
                "alias": {"type": "string", "description": "The alias that was removed."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"internal.aliases_create", ""}};
        example.args = {{"alias", "$internal.aliases_create/alias"}};
        s.examples = {example};
        s.errors = {kAliasNotFound, kAliasStoreFailed};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string alias = utils::normalize_model_name(req.value(kAlias, std::string()));
        try {
            if (!ctx_.alias_manager || !ctx_.alias_manager->remove_alias(alias)) {
                write_alias_error(res, kAliasNotFound, "Alias not found: " + alias);
                return;
            }
            json response = {
                {"status", "deleted"},
                {"alias", alias}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_alias_error(res, kAliasStoreFailed, e.what(), "server_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_delete_route(ServerContext& ctx) {
    return std::make_unique<AliasesDeleteRoute>(ctx);
}

} // namespace lemon
