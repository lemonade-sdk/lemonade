#include <cstdint>
#include <functional>
#include <set>

#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_options.h"
#include "lemon/server/shared_args.h"

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

const RouteError kUnknownOption{400, "", "The body names an option that the model's recipe does not accept"};
const RouteError kLiveProcessOption{
    400, "", "The body sets `pinned`, which belongs to `/v1/load` and `/internal/pin`"};
const RouteError kInvalidOptionValue{
    400, "",
    "`ctx_size` is not a positive whole number or `-1`, or a whole-number option has a fractional value"};

const RouteArg kModelName{"model_name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                          "Ignored; the URL names the model. Accepted so `effective` can be posted back as is."};

class ModelsIdOptionsPostRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "openai.models_id_options_post";
        s.methods = {"POST"};
        s.paths = {"models/{id}/options"};
        s.summary = "Save a model's recipe options";
        s.modality = "n/a";
        s.description =
            "Saves recipe options for a model without loading it. The body is a flat object of "
            "the recipe options [`/v1/load`](./lemonade.md#post-v1load) accepts.";
        s.notes = {
            "The request merges into the model's saved entry, so keys it leaves out keep their "
            "values. `null` removes an option, as do `-1`, `\"\"` and `\"auto\"` for any option "
            "but `ctx_size`, and the model falls back to the next layer of the "
            "[priority chain](./lemonade.md#post-v1load). [`DELETE`](#delete-v1modelsidoptions) "
            "removes every saved option at once.",
            "A request that fails validation saves nothing.",
            "With `dry_run`, the request is validated and resolved as a save would be, but nothing "
            "is persisted: `effective` and `resolved_ctx_size` describe the state the save would "
            "produce, while `saved` keeps reporting the entry on disk.",
            "A running model keeps its current options until it is next loaded.",
            "`pinned` is omitted from `effective` and `defaults`; it belongs to "
            "[`/v1/load`](./lemonade.md#post-v1load) and "
            "[`/internal/pin`](./internal.md#post-internalpin).",
        };
        s.args = {openai::models::kId, model_options::kCtxSize, common::kDryRun, kModelName};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = model_options_schema();
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"id", "Qwen3-4B-GGUF"}, {"ctx_size", 8192}};
        s.examples = {example};
        s.errors = join_errors({{kBodyNotJson, kArgInvalid, kUnknownOption, kLiveProcessOption,
                                 kUnlistedFieldInvalid, kInvalidOptionValue},
                                model_options_errors()});
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        ModelManager* model_manager = ctx_.model_manager;
        respond_with_model_options(ctx_, req.value(openai::models::kId, std::string()), res,
            [model_manager, &req](const std::string& model_key, ModelInfo& info,
                                  httplib::Response& r) {
                json body = req.body;
                const bool dry_run = req.value(common::kDryRun, false);
                body.erase(common::kDryRun.name);

                const auto keys = RecipeOptions::keys_for_recipe(info.recipe);
                const std::set<std::string> allowed(keys.begin(), keys.end());
                const RecipeOptions unset(info.recipe, json::object());

                // Validate everything before writing anything, and express each
                // change as set-or-erase so the merge can happen atomically.
                json changes = json::object();
                for (const auto& [key, value] : body.items()) {
                    if (key == kModelName.name) continue;
                    if (!allowed.count(key)) {
                        write_plain_error(r, kUnknownOption, "Unknown option '" + key +
                                          "' for recipe '" + info.recipe + "'");
                        return false;
                    }
                    if (is_live_process_option(key)) {
                        write_plain_error(r, kLiveProcessOption, "'" + key + "' applies to a running "
                            "model; use /v1/load or /internal/pin");
                        return false;
                    }
                    // null removes the key. The other values the option system
                    // reads as "not set" ("", -1, "auto") still clear every option
                    // except ctx_size, where -1 is a value and a malformed size is
                    // an error rather than an absence.
                    const bool clears = value.is_null() ||
                        (key != model_options::kCtxSize.name && RecipeOptions::is_default_sentinel(key, value));
                    if (clears) {
                        changes[key] = nullptr;
                        continue;
                    }
                    const json expected = unset.get_option(key);
                    if (!option_type_matches(expected, value)) {
                        write_plain_error(r, kUnlistedFieldInvalid,
                                          "Invalid type for option '" + key + "'");
                        return false;
                    }
                    const std::string invalid = validate_option_value(key, expected, value);
                    if (!invalid.empty()) {
                        write_plain_error(r, kInvalidOptionValue, invalid);
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
