#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

#include "lemon/server/adaptive_prompt.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class AdaptivePromptsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.adaptive_prompts";
        s.methods = {"GET"};
        s.paths = {"adaptive-prompts"};
        s.summary = "Browse available adaptive system prompt modules";
        s.description =
            "Lists the trusted server-side adaptive prompt catalog, including the instructions "
            "that an opted-in chat request may select. The catalog is read-only through this API.";
        s.responses = {{ResponseFormat::Json, ordered_json::parse(R"json({
            "type": "object",
            "required": ["source", "count", "modules"],
            "properties": {
                "source": {"enum": ["built-in", "custom"], "description": "Where the catalog was loaded from."},
                "count": {"type": "integer", "description": "Number of available instruction modules."},
                "modules": {"type": "array", "description": "Trusted modules available for selection.", "items": {
                    "type": "object",
                    "required": ["id", "type", "group", "description", "prompt", "keywords"],
                    "properties": {
                        "id": {"type": "string", "description": "Module identifier."},
                        "type": {"enum": ["role", "skill"], "description": "Exclusive primary role or combinable skill."},
                        "group": {"type": "string", "description": "Optional exclusivity group."},
                        "excludes": {"type": "array", "items": {"type": "string"}, "description": "Incompatible module IDs."},
                        "description": {"type": "string", "description": "Task description used for module selection."},
                        "prompt": {"type": "string", "description": "Actual trusted system instructions."},
                        "keywords": {"type": "array", "items": {"type": "string"}, "description": "Rule-selection keywords."}
                    }
                }}
            }
        })json")}};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        try {
            const auto& catalog = adaptive_prompt::catalog();
            json modules = json::array();
            for (const auto& module : catalog) {
                modules.push_back({
                    {"id", module.id},
                    {"type", module.type},
                    {"group", module.group},
                    {"excludes", module.excludes},
                    {"description", module.description},
                    {"prompt", module.prompt},
                    {"keywords", module.keywords},
                });
            }
            const char* custom = std::getenv("LEMONADE_ADAPTIVE_PROMPTS_FILE");
            const json response = {
                {"source", custom && *custom ? "custom" : "built-in"},
                {"count", catalog.size()},
                {"modules", std::move(modules)},
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& error) {
            write_openai_error(res, kUnexpectedException,
                               std::string("Adaptive prompt catalog unavailable: ") + error.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_adaptive_prompts_route(ServerContext& ctx) {
    return std::make_unique<AdaptivePromptsRoute>(ctx);
}

} // namespace lemon
