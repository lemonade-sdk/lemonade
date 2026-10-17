#include "lemon/alias_manager.h"
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kShowAll{"show_all", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
                        "`true` also lists catalog models that are not downloaded. Defaults to `false`, "
                        "which lists downloaded models only."};

class ModelsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models";
        s.methods = {"GET"};
        s.paths = {"models"};
        s.summary = "List the server's models";
        s.modality = "n/a";
        s.description =
            "Lists the models available on the server, in OpenAI's format extended with the "
            "fields described below.";
        s.notes = {
            "A model alias whose target is listed appears too, as a copy of the target with the "
            "alias as its `id`.",
            "With cloud providers configured, their models are listed too, with "
            "`recipe: \"cloud\"`, a `cloud_provider` field and a provider-prefixed id such as "
            "`fireworks.kimi-k2p5`. Models of `openai`-wire providers accept chat completions and "
            "completions requests; `anthropic`-wire providers are served through "
            "[`POST /v1/messages`](./anthropic.md#post-v1messages). See "
            "[Cloud Offload](../guide/configuration/cloud.md).",
            "`HEAD` returns `200 OK` with an empty body.",
        };
        s.args = {kShowAll};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = {
            {"type", "object"},
            {"required", ordered_json::array({"object", "data"})},
            {"properties", {
                {"object", {{"const", "list"},
                            {"description", "Always `\"list\"`."}}},
                {"data", {{"type", "array"},
                          {"description", "The model objects."},
                          {"items", ModelJson::schema()}}},
            }},
        };
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        // OpenAI lists only downloaded models; the CLI's list command asks for all of them.
        bool show_all = req.value(kShowAll, false);

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
