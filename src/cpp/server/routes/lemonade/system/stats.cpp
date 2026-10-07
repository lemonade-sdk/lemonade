#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class StatsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.stats";
        s.methods = {"GET"};
        s.paths = {"stats"};
        s.summary = "Performance statistics from the last request";
        s.description =
            "Reports performance statistics from the last inference request, plus counters "
            "accumulated since the server started.";
        s.notes = {
            "`HEAD` returns `200 OK` with an empty body.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "properties": {
                "time_to_first_token": {"type": ["number", "null"], "description": "Seconds until the first token was generated."},
                "tokens_per_second": {"type": ["number", "null"], "description": "Generation speed."},
                "input_tokens": {"type": ["integer", "null"], "description": "Tokens processed."},
                "output_tokens": {"type": ["integer", "null"], "description": "Tokens generated."},
                "prompt_tokens": {"type": ["integer", "null"], "description": "Prompt tokens, including cached tokens."},
                "cache_tokens": {"type": ["integer", "null"], "description": "Prompt tokens served from the backend's prefix cache: llama.cpp's timings.cache_n, or usage.prompt_tokens_details.cached_tokens (input_tokens_details.cached_tokens for the Responses API) from OpenAI-compatible cloud providers. null when the last request reported no cache usage."},
                "request_count_total": {"type": "integer", "description": "Requests since the server started. The other *_total fields also count since start."},
                "input_tokens_total": {"type": "integer"},
                "output_tokens_total": {"type": "integer"},
                "prompt_tokens_total": {"type": "integer"},
                "cache_tokens_total": {"type": "integer"},
                "routing_decisions_total": {"type": "integer", "description": "Routing decisions made by collection.router dispatch."},
                "routing_switches_total": {"type": "integer", "description": "Routing decisions that changed a conversation's routed model, a proxy for route ping-pong. A conversation is identified by a hash of its system prompt and first user message."}
            }
        })");
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
