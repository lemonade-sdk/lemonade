#include <lemon/utils/aixlog.hpp>

#include "lemon/config_file.h"
#include "lemon/runtime_config.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/config_effects.h"
#include "lemon/utils/json_utils.h"

namespace lemon {
namespace {

using json = nlohmann::json;

std::string json_type_name(const json& value) {
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    return "string";
}

class SetRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.set";
        s.methods = {"POST"};
        s.paths = {"set"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Change config settings at runtime";
        s.description =
            "Changes one or more `config.json` settings without restarting lemond, and saves "
            "them to `config.json`.";
        s.notes = {
            "Every key is validated before any is applied, so one invalid value rejects the "
            "whole request with `400` and changes nothing.",
            "Settings with a runtime effect apply immediately: a `port` or `host` change rebinds "
            "the listeners, `log_level` reconfigures logging, and a backend section's `*_bin` "
            "change reinstalls that backend and reloads its models. The rest apply to the next "
            "model load or eviction decision. See the "
            "[Settings Reference](../guide/configuration/README.md#settings-reference) for every "
            "key.",
            "`config.json` keeps only values that differ from the defaults, so setting a key to "
            "its default removes it from the file. The `lemonade config set` command uses this "
            "endpoint.",
        };

        // The keys are config.json's own, so they come from the canonical defaults and
        // cannot drift from what RuntimeConfig accepts. cloud_providers changes only
        // through /v1/install and /v1/uninstall.
        const json defaults = ConfigFile::base_defaults();
        for (const auto& [key, value] : defaults.items()) {
            if (key == "_generated" || key == "cloud_providers") {
                continue;
            }
            std::string description = value.is_object()
                ? "`" + key + "` settings; send only the keys to change."
                : "Default `" + value.dump() + "`.";
            s.args.push_back({key, ArgIn::JsonBody, {{"type", json_type_name(value)}}, false,
                              Support::Available, description});
        }

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status", "updated"],
            "properties": {
                "status": {"const": "success"},
                "updated": {"type": "object", "description": "The keys the request set, with their new values."},
                "message": {"type": "string", "description": "Present when a legacy key was translated to its current form."}
            }
        })");
        response.example = {{"log_level", "info"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto body = json::parse(req.http.body);

            ConfigEffects* effects = ctx_.config_effects;
            auto result = ctx_.config->set(body, [effects](const json& applied) {
                for (const auto& [key, value] : applied.items()) {
                    effects->apply(key, value);
                }
            });

            const std::string& config_dir = ctx_.server->config_dir();
            if (!config_dir.empty()) {
                try {
                    json user_cfg = ConfigFile::load_raw(config_dir);
                    if (result.contains("updated") && result["updated"].is_object()) {
                        user_cfg = utils::JsonUtils::merge(user_cfg, result["updated"]);
                    }
                    json defaults = ConfigFile::get_defaults();
                    utils::JsonUtils::prune_matching(user_cfg, defaults);
                    ConfigFile::save(config_dir, user_cfg);
                } catch (const std::exception& e) {
                    LOG(WARNING, "Server") << "Failed to persist config.json: " << e.what() << std::endl;
                }
            }

            res.set_content(result.dump(), "application/json");
        } catch (const json::parse_error&) {
            write_plain_error(res, 400, "Invalid JSON in request body");
        } catch (const std::invalid_argument& e) {
            write_plain_error(res, 400, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in /internal/set: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_set_route(ServerContext& ctx) {
    return std::make_unique<SetRoute>(ctx);
}

} // namespace lemon
