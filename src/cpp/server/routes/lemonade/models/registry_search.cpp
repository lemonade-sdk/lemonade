#include <lemon/utils/aixlog.hpp>

#include "lemon/server/model_sync.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kShortQuery{
    400, "", "`query` (or `q`) is shorter than 3 characters after trimming"};
const RouteError kInvalidSearchParam{
    400, "",
    "`source` names an unsupported registry, `limit` is not an integer from 1 to 50, or "
    "`format` is not `gguf`"};
const RouteError kRateLimited{
    429, "",
    "The upstream registry rate-limits the request; `error.upstream_status_code` is its "
    "status"};
const RouteError kUpstreamError{
    502, "",
    "The upstream registry answers with another error; `error.upstream_status_code` is its "
    "status"};
const RouteError kSearchFailed{
    502, "", "The search fails with no upstream status, for example on a network error"};

const RouteArg kQuery{"query", ArgIn::Query, {{"type", "string"}}, true, Support::Available,
                      "Search text, minimum 3 characters after trimming.",
                      {"q"}};
const RouteArg kSource{"source", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                       "Registry to search: `huggingface` or `modelscope`, also accepted as `hf` and `ms`. "
                       "Defaults to `huggingface`."};
const RouteArg kLimit{"limit", ArgIn::Query, {{"type", "integer"}}, false, Support::Available,
                      "Maximum number of results, from `1` to `50`. Defaults to `12`."};
const RouteArg kFormat{"format", ArgIn::Query, {{"enum", json::array({"gguf"})}}, false, Support::Available,
                       "Only `gguf`, which limits a Hugging Face search to GGUF repositories and favors them "
                       "in a ModelScope search."};

class RegistrySearchRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.registry_search";
        s.methods = {"GET"};
        s.paths = {"registry/search"};
        s.summary = "Search Hugging Face or ModelScope for model repositories";
        s.description =
            "Searches Hugging Face or ModelScope for repositories matching a text query. The "
            "results are candidates chosen from registry metadata, so the desktop app's Model "
            "Manager checks each one with [`GET /v1/pull/variants`](#get-v1pullvariants) and offers a "
            "download only for repositories whose files pass.";
        s.args = {kQuery, kSource, kLimit, kFormat};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["source", "query", "total", "results"],
            "properties": {
                "source": {"enum": ["huggingface", "modelscope"], "description": "The registry searched, by its canonical name."},
                "query": {"type": "string", "description": "The query, trimmed."},
                "format": {"const": "gguf", "description": "Present only when `format=gguf` was requested."},
                "total": {"type": "integer", "description": "Matches the registry reports; can exceed the results returned."},
                "results": {"type": "array", "description": "Up to `limit` repositories.", "items": {
                    "type": "object",
                    "required": ["repository_id", "display_name", "source"],
                    "properties": {
                        "repository_id": {"type": "string", "description": "Repository id, such as `unsloth/Qwen3-8B-GGUF`."},
                        "display_name": {"type": "string", "description": "Name to show for the repository."},
                        "source": {"type": "string", "description": "The registry the repository is on."},
                        "repository_type": {"type": "string", "description": "The registry's repository type."},
                        "description": {"type": "string", "description": "The repository's description."},
                        "tags": {"type": "array", "items": {"type": "string"}, "description": "The repository's tags."},
                        "task": {"type": "string", "description": "The repository's task, such as `text-generation`."},
                        "downloads": {"type": "integer", "description": "Download count the registry reports."},
                        "likes": {"type": "integer", "description": "Like count the registry reports."},
                        "has_gguf": {"type": "boolean", "description": "A hint from registry metadata."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"source", "modelscope"}, {"query", "qwen"}, {"format", "gguf"}};
        s.examples = {example};
        s.errors = {kShortQuery, kInvalidSearchParam, kLemondOffline,
                    kRateLimited, kUpstreamError, kSearchFailed};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string query = req.value(kQuery, "");
            const auto first = query.find_first_not_of(" \t\r\n");
            const auto last = query.find_last_not_of(" \t\r\n");
            query = first == std::string::npos ? "" : query.substr(first, last - first + 1);
            if (query.size() < 3) {
                write_openai_error(res, kShortQuery,
                                   "Query parameter 'query' must contain at least 3 characters");
                return;
            }

            const std::string source_text = req.value(kSource, "huggingface");
            const auto source = parse_remote_registry_source(source_text);

            std::size_t limit = 12;
            if (req.has(kLimit)) {
                const json& limit_arg = req.arg(kLimit);
                std::size_t parsed = 0;
                if (limit_arg.is_number_integer()) {
                    parsed = limit_arg.get<std::size_t>();
                } else {
                    const std::string limit_text = limit_arg.get<std::string>();
                    try {
                        std::size_t consumed = 0;
                        parsed = static_cast<std::size_t>(std::stoul(limit_text, &consumed));
                        if (consumed != limit_text.size()) throw std::invalid_argument("trailing characters");
                    } catch (...) {
                        throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
                    }
                }
                if (parsed < 1 || parsed > 50) {
                    throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
                }
                limit = parsed;
            }

            bool gguf_only = false;
            if (req.has(kFormat)) {
                const std::string format = req.arg(kFormat).get<std::string>();
                if (format != "gguf") {
                    throw std::invalid_argument(
                        "Unsupported registry search format '" + format + "' (expected 'gguf')");
                }
                gguf_only = true;
            }

            if (ctx_.config->offline()) {
                write_openai_error(res, kLemondOffline,
                                   "Lemond is in offline mode, registry search is unavailable");
                return;
            }

            const auto search = search_registry_models(source, query, limit, gguf_only);
            json results = json::array();
            for (const auto& model : search.results) {
                results.push_back({
                    {"repository_id", model.repo_id},
                    {"display_name", model.display_name},
                    {"source", remote_registry_source_name(model.source)},
                    {"repository_type", model.repository_type},
                    {"description", model.description},
                    {"tags", model.tags},
                    {"task", model.task},
                    {"downloads", model.downloads},
                    {"likes", model.likes},
                    {"has_gguf", model.has_gguf}
                });
            }
            json response = {
                {"source", remote_registry_source_name(source)},
                {"query", query},
                {"total", search.total},
                {"results", std::move(results)}
            };
            if (gguf_only) response["format"] = "gguf";
            res.set_content(response.dump(), "application/json");
        } catch (const RegistrySearchError& e) {
            res.status = e.status_code() == kRateLimited.status ? kRateLimited.status
                                                                : kUpstreamError.status;
            res.set_content(json{{"error", {
                {"message", e.what()},
                {"type", "registry_error"},
                {"upstream_status_code", e.status_code()}
            }}}.dump(), "application/json");
        } catch (const std::invalid_argument& e) {
            write_openai_error(res, kInvalidSearchParam, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_registry_search: " << e.what() << std::endl;
            write_openai_error(res, kSearchFailed, e.what(), "registry_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_registry_search_route(ServerContext& ctx) {
    return std::make_unique<RegistrySearchRoute>(ctx);
}

} // namespace lemon
