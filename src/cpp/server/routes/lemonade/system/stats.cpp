#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class StatsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.stats";
        s.methods = {"GET"};
        s.paths = {"stats"};
        s.summary = "Performance statistics from the last request";
        s.description = "Performance statistics from the last request.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"json({
            "type": "object",
            "properties": {
                "time_to_first_token": {"type": ["number", "null"], "description": "Time in seconds until the first token was generated"},
                "tokens_per_second": {"type": ["number", "null"], "description": "Generation speed in tokens per second"},
                "input_tokens": {"type": ["integer", "null"], "description": "Number of tokens processed"},
                "output_tokens": {"type": ["integer", "null"], "description": "Number of tokens generated"},
                "prompt_tokens": {"type": ["integer", "null"], "description": "Total prompt tokens including cached tokens"},
                "cache_tokens": {"type": ["integer", "null"], "description": "Prompt tokens served from the backend's prefix cache on the last request (llama.cpp `timings.cache_n`, or `usage.prompt_tokens_details.cached_tokens` / Responses-API `input_tokens_details.cached_tokens` from OpenAI-compatible cloud providers). `null` when the last request did not report cache usage"},
                "request_count_total": {"type": "integer", "description": "Cumulative counters since server start"},
                "input_tokens_total": {"type": "integer", "description": "Cumulative counters since server start"},
                "output_tokens_total": {"type": "integer", "description": "Cumulative counters since server start"},
                "prompt_tokens_total": {"type": "integer", "description": "Cumulative counters since server start"},
                "cache_tokens_total": {"type": "integer", "description": "Cumulative counters since server start"},
                "routing_decisions_total": {"type": "integer", "description": "Routing decisions made by `collection.router` dispatch"},
                "routing_switches_total": {"type": "integer", "description": "Routing decisions that changed a conversation's routed model (a proxy for route ping-pong; conversations are identified by a hash of the system prompt and first user message)"}
            }
        })json");
        response.setup = {{"openai.chat_completions", ResponseFormat::Json}};
        response.example = json::object();
        s.responses = {response};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        if (req.http.method == "HEAD") {
            res.status = 200;
            return;
        }

        try {
            auto stats = ctx_.router->get_stats();
            res.set_content(stats.dump(), "application/json");
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_stats: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_stats_route(ServerContext& ctx) {
    return std::make_unique<StatsRoute>(ctx);
}

} // namespace lemon
