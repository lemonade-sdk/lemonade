#include "alias_errors.h"
#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kBlankField{400, "invalid_request", "`alias` or `target` is blank"};
const RouteError kSelfAlias{400, "invalid_alias", "`alias` equals `target`"};
const RouteError kReservedPrefix{
    400, "invalid_alias", "`alias` starts with `user.`, `extra.` or `builtin.`"};
const RouteError kNameConflict{409, "alias_conflict", "`alias` is already a registered model's name"};
const RouteError kAliasChain{
    409, "alias_conflict",
    "The alias would form a cycle or an alias chain deeper than 10 links"};

const RouteArg kAlias{"alias", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                      "The alias name to create or update."};
const RouteArg kTarget{"target", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                       "The model name, canonical ID or other alias to point at, which is not checked to "
                       "exist.", {"model"}};

class AliasesCreateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.aliases_create";
        s.methods = {"POST"};
        s.paths = {"aliases"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Create or update a model alias";
        s.description =
            "Points an alias at a target model, replacing any target it had. The "
            "`lemonade alias add` command uses it.";
        s.args = {kAlias, kTarget};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "alias", "target"],
            "properties": {
                "status": {"const": "ok", "description": "Always `ok`."},
                "alias": {"type": "string", "description": "The alias name."},
                "target": {"type": "string", "description": "Model or alias the alias now points to."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"alias", "my-alias-1"}, {"target", "user.custom-llama"}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kBlankField, kSelfAlias, kReservedPrefix,
                    kNameConflict, kAliasChain, kAliasStoreFailed};
        s.error_format = ErrorFormat::OpenAi;
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string alias = utils::normalize_model_name(req.value(kAlias, ""));
        std::string target = utils::normalize_model_name(req.value(kTarget, ""));

        if (alias.empty() || target.empty()) {
            write_alias_error(res, kBlankField, "Alias and target fields are required");
            return;
        }

        if (alias == target) {
            write_alias_error(res, kSelfAlias, "Alias cannot point to itself");
            return;
        }

        if (alias.rfind("user.", 0) == 0 || alias.rfind("extra.", 0) == 0 || alias.rfind("builtin.", 0) == 0) {
            write_alias_error(res, kReservedPrefix, "Alias name cannot use reserved prefixes (user., extra., builtin.)");
            return;
        }

        if (ctx_.model_manager->model_exists(alias)) {
            std::string canonical = ctx_.model_manager->resolve_model_name(alias);
            if (canonical == alias || canonical == "user." + alias || canonical == "builtin." + alias) {
                write_alias_error(res, kNameConflict, "Cannot create alias '" + alias + "': Name conflicts with an existing canonical model");
                return;
            }
        }

        if (!ctx_.alias_manager) {
            write_alias_error(res, kAliasStoreFailed, "AliasManager uninitialized", "server_error");
            return;
        }

        std::string err_msg;
        if (!ctx_.alias_manager->set_alias(alias, target, err_msg)) {
            write_alias_error(res, kAliasChain, err_msg);
            return;
        }

        json response = {
            {"status", "ok"},
            {"alias", alias},
            {"target", target}
        };
        res.set_content(response.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_create_route(ServerContext& ctx) {
    return std::make_unique<AliasesCreateRoute>(ctx);
}

} // namespace lemon
