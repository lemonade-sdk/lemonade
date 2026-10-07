#include <cstdint>
#include <functional>
#include <set>

#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"

namespace lemon {

namespace {

using json = nlohmann::json;

// `pinned` is live-process state: Router::load_model keeps a running server's
// own pin rather than the resolved one, so a value saved here would not reach a
// model that is already up. /v1/load and /internal/pin own it.
bool is_live_process_option(const std::string& key) {
    return key == "pinned";
}

bool option_type_matches(const json& expected, const json& value) {
    if (expected.is_number()) return value.is_number();
    if (expected.is_string()) return value.is_string();
    // Booleans, and the tri-state options whose default is null to mean "follow
    // the global setting", both only ever hold a boolean once set.
    return value.is_boolean();
}

// Reject values that pass the type check but would break the load they are
// saved for. Returns an error message, or empty when the value is usable.
std::string validate_option_value(const std::string& key, const json& expected,
                                  const json& value) {
    if (key == "ctx_size") {
        // A literal above INT64_MAX parses as an unsigned and wraps on the way
        // to int64_t, so 2^64-1 would arrive as -1 and read as "size it
        // automatically". Rule it out before the conversion.
        const bool fits_int64 = value.is_number_integer() &&
            (!value.is_number_unsigned() ||
             value.get<uint64_t>() <= static_cast<uint64_t>(INT64_MAX));
        // 0 is not a shorthand for anything here: only -1 auto-resolves, so a 0
        // reaches the backend verbatim and FastFlowLM and vLLM both reject it.
        if (!fits_int64 || value.get<int64_t>() < -1 || value == 0) {
            return "'ctx_size' must be a positive whole number, "
                   "or -1 to size it automatically";
        }
        return "";
    }

    // A fractional value for a whole-number option is silently ignored by the
    // consumers that read it, so refuse it.
    if (expected.is_number_integer() && !value.is_number_integer()) {
        return "'" + key + "' must be a whole number";
    }
    return "";
}

class ModelsIdOptionsPostRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_options_post";
        s.methods = {"POST"};
        s.paths = {"models/{id}/options"};
        s.summary = "Save recipe options for a model without loading it";
        s.description =
            "Lemonade extension: saves recipe options for a model without loading it. The body is "
            "a flat object of the recipe options [`/v1/load`](./lemonade.md#post-v1load) accepts, "
            "and the response is the same as "
            "[`GET /v1/models/{id}/options`](#get-v1modelsidoptions), after the write.";
        s.notes = {
            "The request merges into the model's saved entry, so options it does not mention keep "
            "their saved values. `null` removes an option, and the model falls back to the next "
            "layer of the [priority chain](./lemonade.md#post-v1load). "
            "[`DELETE`](#delete-v1modelsidoptions) removes every saved option at once.",
            "A `400` reports an unrecognized option name, an option from a different recipe, a "
            "value of the wrong type, or an invalid `ctx_size`, and nothing from that request is "
            "saved.",
            "Saving never loads or reloads the model, so a model that is already running keeps "
            "its current options until it is next loaded.",
            "`pinned` is not settable here and is omitted from `effective` and `defaults`. It "
            "belongs to [`/v1/load`](./lemonade.md#post-v1load) and "
            "[`/internal/pin`](./internal.md#post-internalpin).",
        };
        s.args = {
            {"id", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::Available,
             "Model id, as listed by [`GET /v1/models`](#get-v1models), or an alias."},
            {"ctx_size", ArgIn::JsonBody, {{"type", json::array({"integer", "null"})}}, false, Support::Available,
             "A positive whole number, or `-1` to pin the model to automatic sizing even when the "
             "server-wide `ctx_size` is a specific number. Any other option the model's recipe "
             "accepts is set the same way."},
            {"dry_run", ArgIn::JsonBody, {{"type", "boolean"}}, false, Support::Available,
             "`true` validates and resolves the request identically but persists nothing: "
             "`effective` and `resolved_ctx_size` describe the state the save would produce, while "
             "`saved` keeps reporting the entry on disk."},
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Ignored; the URL names the model. Accepted so `effective` can be posted back as is."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_options_schema();
        response.example = {{"id", "Qwen3-4B-GGUF"}, {"ctx_size", 8192}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager* model_manager = ctx_.model_manager;
        const httplib::Request& http = req.http;
        // The body is parsed after the model resolves, so an unknown model answers 404
        // even when the body is also invalid.
        respond_with_model_options(ctx_, http, res,
            [model_manager, &http](const std::string& model_key, ModelInfo& info,
                                   httplib::Response& r) {
                if (http.body.empty()) {
                    write_plain_error(r, 400, "Request body is required but was empty");
                    return false;
                }
                json body;
                try {
                    body = json::parse(http.body);
                } catch (const json::exception& e) {
                    write_plain_error(r, 400, std::string("Invalid JSON in request body: ") + e.what());
                    return false;
                }
                if (!body.is_object()) {
                    write_plain_error(r, 400, "Request body must be a JSON object of recipe options");
                    return false;
                }

                bool dry_run = false;
                if (body.contains("dry_run")) {
                    if (!body["dry_run"].is_boolean()) {
                        write_plain_error(r, 400, "'dry_run' must be a boolean");
                        return false;
                    }
                    dry_run = body["dry_run"].get<bool>();
                    body.erase("dry_run");
                }

                const auto keys = RecipeOptions::keys_for_recipe(info.recipe);
                const std::set<std::string> allowed(keys.begin(), keys.end());
                const RecipeOptions unset(info.recipe, json::object());

                // Validate everything before writing anything, and express each
                // change as set-or-erase so the merge can happen atomically.
                json changes = json::object();
                for (const auto& [key, value] : body.items()) {
                    if (key == "model_name") continue;
                    if (!allowed.count(key)) {
                        write_plain_error(r, 400, "Unknown option '" + key +
                                                      "' for recipe '" + info.recipe + "'");
                        return false;
                    }
                    if (is_live_process_option(key)) {
                        write_plain_error(r, 400, "'" + key + "' applies to a running "
                            "model; use /v1/load or /internal/pin");
                        return false;
                    }
                    // null removes the key. The other values the option system
                    // reads as "not set" ("", -1, "auto") still clear every option
                    // except ctx_size, where -1 is a value and a malformed size is
                    // an error rather than an absence.
                    const bool clears = value.is_null() ||
                        (key != "ctx_size" && RecipeOptions::is_default_sentinel(key, value));
                    if (clears) {
                        changes[key] = nullptr;
                        continue;
                    }
                    const json expected = unset.get_option(key);
                    if (!option_type_matches(expected, value)) {
                        write_plain_error(r, 400, "Invalid type for option '" + key + "'");
                        return false;
                    }
                    const std::string invalid = validate_option_value(key, expected, value);
                    if (!invalid.empty()) {
                        write_plain_error(r, 400, invalid);
                        return false;
                    }
                    changes[key] = value;
                }

                if (dry_run) {
                    info.recipe_options = model_manager->preview_saved_model_options(info, changes);
                    return true;
                }
                if (!changes.empty()) {
                    model_manager->update_saved_model_options(model_key, changes);
                }
                info = model_manager->get_model_info(model_key);
                return true;
            });
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_models_id_options_post_route(ServerContext& ctx) {
    return std::make_unique<ModelsIdOptionsPostRoute>(ctx);
}

} // namespace lemon
