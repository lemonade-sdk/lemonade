#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/utils/model_name_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

// Alias failures are not model-lookup failures. ModelLoader::model_error() reports any
// name missing from the registry as "model not found", which a new alias always is.
void write_alias_error(httplib::Response& res, int status, const std::string& message,
                       const std::string& code,
                       const std::string& type = "invalid_request_error") {
    res.status = status;
    res.set_content(json{{"error", {
        {"message", message},
        {"type", type},
        {"param", "alias"},
        {"code", code}
    }}}.dump(), "application/json");
}

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
        s.description = "Points a model alias at a target model, creating or updating it.";
        s.notes = {
            "A `400` answers a missing `alias` or `target`, an alias equal to its target, or an "
            "alias with a reserved prefix (`user.`, `extra.`, `builtin.`). A `409` answers an "
            "alias that is already a model's canonical name, or one that would form a cycle.",
            "The `lemonade alias add` command uses this endpoint.",
        };
        s.args = {
            {"alias", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Alias name to create or update."},
            {"target", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model name or canonical ID the alias points to. `model` is accepted as an alias."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status", "alias", "target"],
            "properties": {
                "status": {"const": "ok"},
                "alias": {"type": "string"},
                "target": {"type": "string"}
            }
        })");
        response.example = {{"alias", "my-chat-model"}, {"target", "Qwen3-0.6B-GGUF"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto req_json = json::parse(req.http.body);
            std::string alias = utils::normalize_model_name(req_json.value("alias", ""));
            std::string target = utils::normalize_model_name(req_json.value("target", req_json.value("model", "")));

            if (alias.empty() || target.empty()) {
                write_alias_error(res, 400, "Alias and target fields are required", "invalid_request");
                return;
            }

            if (alias == target) {
                write_alias_error(res, 400, "Alias cannot point to itself", "invalid_alias");
                return;
            }

            if (alias.rfind("user.", 0) == 0 || alias.rfind("extra.", 0) == 0 || alias.rfind("builtin.", 0) == 0) {
                write_alias_error(res, 400, "Alias name cannot use reserved prefixes (user., extra., builtin.)", "invalid_alias");
                return;
            }

            if (ctx_.model_manager->model_exists(alias)) {
                std::string canonical = ctx_.model_manager->resolve_model_name(alias);
                if (canonical == alias || canonical == "user." + alias || canonical == "builtin." + alias) {
                    write_alias_error(res, 409, "Cannot create alias '" + alias + "': Name conflicts with an existing canonical model", "alias_conflict");
                    return;
                }
            }

            std::string err_msg;
            if (!ctx_.alias_manager->set_alias(alias, target, err_msg)) {
                write_alias_error(res, 409, err_msg, "alias_conflict");
                return;
            }

            json response = {
                {"status", "ok"},
                {"alias", alias},
                {"target", target}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            write_alias_error(res, 400, e.what(), "invalid_request");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_aliases_create_route(ServerContext& ctx) {
    return std::make_unique<AliasesCreateRoute>(ctx);
}

} // namespace lemon
