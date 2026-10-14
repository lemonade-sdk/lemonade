#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class ModelsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models";
        s.methods = {"GET"};
        s.paths = {"models"};
        s.summary = "List models available locally";
        s.modality = "n/a";
        s.description =
            "Returns a list of models available on the server in an OpenAI-compatible format. "
            "Each model object includes extended fields like `checkpoint`, `recipe`, `size`, "
            "`downloaded`, `labels`, `context_length`, and, when known, `max_context_window`.\n\n"
            "By default, only models available locally (downloaded) are shown, matching OpenAI "
            "API behavior.\n\n"
            "When `lemond` is configured with cloud providers, cloud-routed models appear here "
            "alongside local ones with `recipe: \"cloud\"` and a `cloud_provider` field. They are "
            "dot-namespaced by provider (e.g. `fireworks.kimi-k2p5`) and accept the standard "
            "chat-completions / completions requests below — see "
            "[Cloud Offload](../guide/configuration/cloud.md).";
        s.notes = {
            "Model aliases are listed too, each as a copy of its target model with the alias as "
            "its `id`.",
            "`HEAD` answers `200` with no body.",
        };
        s.args = {
            {"show_all", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
             "If set to `true`, returns all models from the catalog including those not yet "
             "downloaded. Defaults to `false`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"object", "data"})},
            {"properties", {
                {"object", {{"const", "list"},
                            {"description", "Type of response object, always `\"list\"`"}}},
                {"data", {{"type", "array"},
                          {"description", "Array of model objects with the following fields:"},
                          {"items", ModelJson::schema()}}},
            }},
        };
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        // OpenAI lists only downloaded models; the CLI's list command asks for all of them.
        bool show_all = req.http.has_param("show_all") &&
                        req.http.get_param_value("show_all") == "true";

        std::map<std::string, ModelInfo> models;
        if (show_all) {
            models = ctx_.model_manager->get_supported_models();
        } else {
            models = ctx_.model_manager->get_downloaded_models();
        }

        json response;
        response["data"] = json::array();
        response["object"] = "list";

        for (const auto& [model_id, model_info] : models) {
            response["data"].push_back(ctx_.model_json->to_json(model_id, model_info));
        }

        auto all_aliases = ctx_.alias_manager->get_all_aliases();
        for (const auto& [alias_id, target_name] : all_aliases) {
            if (models.count(alias_id) > 0) {
                continue;
            }
            std::string ultimate_target = target_name;
            if (auto resolved = ctx_.alias_manager->resolve_alias(alias_id)) {
                ultimate_target = *resolved;
            }
            std::string canonical_target = ctx_.model_manager->resolve_model_name(ultimate_target);
            if (models.count(canonical_target) > 0) {
                response["data"].push_back(
                    ctx_.model_json->to_json(alias_id, models.at(canonical_target)));
            } else if (models.count(ultimate_target) > 0) {
                response["data"].push_back(
                    ctx_.model_json->to_json(alias_id, models.at(ultimate_target)));
            }
        }

        res.set_content(response.dump(), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_route(ServerContext& ctx) {
    return std::make_unique<ModelsRoute>(ctx);
}

} // namespace lemon
