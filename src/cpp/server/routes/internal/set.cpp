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
using ordered_json = nlohmann::ordered_json;

std::string json_type_name(const json& value) {
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    return "string";
}

const RouteError kInvalidSetting{
    400, "", "The body is `{}`, names an unknown setting, or gives a setting a value outside its range or options"};

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
            "Changes one or more `config.json` settings without restarting `lemond`, and saves "
            "them. The `lemonade config set` command uses it.";
        s.notes = {
            "Every key is validated before any is applied, so an unknown key or one invalid value "
            "rejects the whole request and changes nothing.",
            "Settings with a runtime effect apply immediately, such as `port` and `host`, which "
            "rebind the listeners, the logging settings, and a backend section's `*_bin`, which "
            "reinstalls that backend and reloads its models. Other settings are read the next "
            "time they are needed, such as at the next model load. See the "
            "[Settings Reference](../guide/configuration/README.md#settings-reference) for every "
            "key.",
            "`config.json` keeps only values that differ from the defaults, so setting a key to "
            "its default removes it from the file.",
        };

        // The keys are config.json's own, so they come from the canonical defaults and
        // cannot drift from what RuntimeConfig accepts. cloud_providers changes only
        // through /v1/install and /v1/uninstall.
        const ordered_json defaults = ConfigFile::base_defaults();
        for (const auto& [key, value] : defaults.items()) {
            if (key == "_generated" || key == "cloud_providers") {
                continue;
            }
            std::string description = value.is_object()
                ? "`" + key + "` settings; send only the keys to change."
                : "Defaults to `" + value.dump() + "`.";
            // "auto" picks a free port; a number pins it.
            json type = key == "websocket_port" ? json::array({"string", "integer"})
                                                : json(json_type_name(value));
            s.args.push_back({key, ArgIn::JsonBody, {{"type", type}}, false, Support::Available,
                              description});
        }

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "updated"],
            "properties": {
                "status": {"const": "success", "description": "Always `success`."},
                "updated": {"type": "object", "description": "The keys the request set, with their new values, in `config.json`'s nested form: a flat key such as `llamacpp_backend` appears under `llamacpp`."},
                "message": {"type": "string", "description": "Present when a deprecated value was replaced, such as `rocm_channel=preview`."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"log_level", "info"}};
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgInvalid, kInvalidSetting, kUnexpectedException};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            ConfigEffects* effects = ctx_.config_effects;
            auto result = ctx_.config->set(req.body, [effects](const json& applied) {
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
        } catch (const std::invalid_argument& e) {
            write_plain_error(res, kInvalidSetting, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in /internal/set: " << e.what() << std::endl;
            write_plain_error(res, kUnexpectedException, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_set_route(ServerContext& ctx) {
    return std::make_unique<SetRoute>(ctx);
}

} // namespace lemon
