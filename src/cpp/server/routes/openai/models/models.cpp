#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"
#include "lemon/server/model_json.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteArg kShowAll{"show_all", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
                        "`true` also lists catalog models that are not downloaded. Defaults to `false`, "
                        "which lists downloaded models only."};

// Canonical discovery parameters. show_all and include_paths retain their old behavior.
const RouteArg kQuery{"query", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                      "Case-insensitive AND-token search across id, checkpoints, recipe and labels."};
const RouteArg kCapability{"capability", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                           "Server-owned capability, e.g. chat, image-edit, router or embedding. "
                           "Unknown capabilities return 400."};
const RouteArg kDownloaded{"downloaded", ArgIn::Query, {{"type", "boolean"}}, false, Support::Available,
                           "Filter by downloaded state; overrides the legacy show_all default."};
const RouteArg kLimit{"limit", ArgIn::Query, {{"type", "integer"}, {"minimum", 1}}, false, Support::Available,
                      "Positive page size. Without it, results are unbounded."};
const RouteArg kCursor{"cursor", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                       "Opaque continuation cursor from next_cursor."};
const RouteError kInvalidDiscoveryQuery{400, "invalid_model_query",
                                       "The capability, limit or cursor is invalid"};

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
            "Canonical discovery uses query, capability, downloaded, limit and cursor. "
            "It excludes aliases, reports matching_total and next_cursor, and searches "
            "only models supported by the running server.",
        };
        s.args = {kShowAll, kQuery, kCapability, kDownloaded, kLimit, kCursor,
                  openai::models::kIncludePaths};

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
                {"matching_total", {{"type", "integer"}, {"description", "Count of matching real model records; present for canonical discovery only."}}},
                {"next_cursor", {{"type", {"string", "null"}}, {"description", "Next page cursor, or null; present for canonical discovery only."}}},
            }},
        };
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.errors = {kInvalidDiscoveryQuery, ApiRoute::kArgInvalid};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        // Canonical queries return only registered supported records, never alias
        // copies. The original list path still includes aliases for OpenAI/CLI users.
        const bool show_all = req.value(kShowAll, false);
        const bool include_paths = req.value(openai::models::kIncludePaths, false);
        const bool canonical = req.has(kQuery) || req.has(kCapability) ||
                               req.has(kDownloaded) || req.has(kLimit) || req.has(kCursor);

        json response = {{"data", json::array()}, {"object", "list"}};
        if (!canonical) {
            const auto models = show_all ? ctx_.model_manager->get_supported_models()
                                         : ctx_.model_manager->get_downloaded_models();
            for (const auto& [row_id, info] : ctx_.model_manager->listing_rows(models)) {
                response["data"].push_back(
                    ctx_.model_json->to_json(row_id, *info, 0, include_paths));
            }
        } else {
            ModelQueryOptions options;
            options.query = req.value(kQuery, std::string());
            options.capability = req.value(kCapability, std::string());
            options.cursor = req.value(kCursor, std::string());
            if (req.has(kDownloaded)) options.downloaded = req.value(kDownloaded, false);
            else if (!show_all) options.downloaded = true;

            // ApiRoute converts in-range integers but preserves an overflowing
            // integer as a string, so handle both forms without raising a 500.
            if (req.has(kLimit)) {
                const json& limit_arg = req.arg(kLimit);
                const std::string value = limit_arg.is_string()
                    ? limit_arg.get<std::string>() : limit_arg.dump();
                try {
                    if (value.empty() || !std::all_of(value.begin(), value.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; })) {
                        throw std::invalid_argument("invalid digits");
                    }
                    const auto parsed = std::stoull(value);
                    if (parsed == 0 || parsed >
                        static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                        throw std::invalid_argument("out of range");
                    }
                    options.limit = static_cast<size_t>(parsed);
                } catch (const std::exception&) {
                    write_plain_error(res, kInvalidDiscoveryQuery,
                                      "'limit' must be a positive integer");
                    return;
                }
            }

            try {
                const ModelQueryResult page = ctx_.model_manager->query_models(options);
                for (const auto& [row_id, info] : page.models) {
                    response["data"].push_back(
                        ctx_.model_json->to_json(row_id, info, 0, include_paths));
                }
                response["matching_total"] = page.matching_total;
                response["next_cursor"] = page.next_cursor.empty()
                    ? json(nullptr) : json(page.next_cursor);
            } catch (const std::invalid_argument& e) {
                write_plain_error(res, kInvalidDiscoveryQuery, e.what());
                return;
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
