#include <lemon/utils/aixlog.hpp>

#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Manager checks each one with [`/v1/pull/variants`](#get-v1pullvariants) and offers a "
            "download only for repositories whose files pass.";
        s.notes = {
            "A `400` answers a `query` shorter than 3 characters, an invalid `source`, `limit` or "
            "`format`, or offline mode (`code: lemond_offline`). A `429` means the upstream "
            "registry rate-limited the request, and any other upstream failure answers `502`, with "
            "the upstream status code when available.",
        };
        s.args = {
            {"query", ArgIn::Query, {{"type", "string"}}, true, Support::Available,
             "Search text, minimum 3 characters after trimming. `q` is accepted as an alias."},
            {"source", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
             "Registry to search: `huggingface` or `modelscope`, also accepted as `hf` and `ms`. "
             "Defaults to `huggingface`."},
            {"limit", ArgIn::Query, {{"type", "integer"}}, false, Support::Available,
             "Maximum number of results, from `1` to `50`. Defaults to `12`."},
            {"format", ArgIn::Query, {{"enum", json::array({"gguf"})}}, false, Support::Available,
             "Only `gguf`, which favors GGUF repositories in search and ranking."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["source", "query", "total", "results"],
            "properties": {
                "source": {"enum": ["huggingface", "modelscope"], "description": "The registry searched, by its canonical name."},
                "query": {"type": "string", "description": "The query as sent."},
                "format": {"const": "gguf", "description": "Present only when `format=gguf` was requested."},
                "total": {"type": "integer", "description": "Matches the registry reports; can exceed the results returned."},
                "results": {"type": "array", "description": "Up to `limit` repositories.", "items": {
                    "type": "object",
                    "required": ["repository_id", "display_name", "source"],
                    "properties": {
                        "repository_id": {"type": "string"},
                        "display_name": {"type": "string"},
                        "source": {"type": "string"},
                        "repository_type": {"type": "string"},
                        "description": {"type": "string"},
                        "tags": {"type": "array", "items": {"type": "string"}},
                        "task": {"type": "string"},
                        "downloads": {"type": "integer"},
                        "likes": {"type": "integer"},
                        "has_gguf": {"type": "boolean", "description": "A hint from registry metadata; [`/v1/pull/variants`](#get-v1pullvariants) checks the files."}
                    }
                }}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"source", "modelscope"}, {"query", "qwen"}, {"format", "gguf"}};
        s.examples = {example};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const httplib::Request& http = req.http;
        try {
            std::string query = http.has_param("query")
                ? http.get_param_value("query")
                : (http.has_param("q") ? http.get_param_value("q") : "");
            const auto first = query.find_first_not_of(" \t\r\n");
            const auto last = query.find_last_not_of(" \t\r\n");
            query = first == std::string::npos ? "" : query.substr(first, last - first + 1);
            if (query.size() < 3) {
                write_openai_error(res, 400, "Query parameter 'query' must contain at least 3 characters");
                return;
            }

            const std::string source_text = http.has_param("source")
                ? http.get_param_value("source") : "huggingface";
            const auto source = parse_remote_registry_source(source_text);

            std::size_t limit = 12;
            if (http.has_param("limit")) {
                const std::string limit_text = http.get_param_value("limit");
                std::size_t parsed = 0;
                try {
                    std::size_t consumed = 0;
                    parsed = static_cast<std::size_t>(std::stoul(limit_text, &consumed));
                    if (consumed != limit_text.size()) throw std::invalid_argument("trailing characters");
                } catch (...) {
                    throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
                }
                if (parsed < 1 || parsed > 50) {
                    throw std::invalid_argument("Query parameter 'limit' must be an integer from 1 to 50");
                }
                limit = parsed;
            }

            bool gguf_only = false;
            if (http.has_param("format")) {
                const std::string format = http.get_param_value("format");
                if (format != "gguf") {
                    throw std::invalid_argument(
                        "Unsupported registry search format '" + format + "' (expected 'gguf')");
                }
                gguf_only = true;
            }

            if (ctx_.config->offline()) {
                write_openai_error(res, 400, "Lemond is in offline mode, registry search is unavailable",
                                   "invalid_request_error", "lemond_offline");
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
            res.status = e.status_code() == 429 ? 429 : 502;
            res.set_content(json{{"error", {
                {"message", e.what()},
                {"type", "registry_error"},
                {"upstream_status_code", e.status_code()}
            }}}.dump(), "application/json");
        } catch (const std::invalid_argument& e) {
            write_openai_error(res, 400, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_registry_search: " << e.what() << std::endl;
            write_openai_error(res, 502, e.what(), "registry_error");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_registry_search_route(ServerContext& ctx) {
    return std::make_unique<RegistrySearchRoute>(ctx);
}

} // namespace lemon
