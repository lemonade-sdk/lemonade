#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/server/api_route.h"

#include <sstream>

namespace lemon {
namespace {

using json = nlohmann::json;

class BackendSchemaRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.backends_schema";
        s.methods = {"GET"};
        s.paths = {"backends/schema"};
        return s;
    }
    void handle(RouteRequest& request, httplib::Response& res) override {
        const auto& req = request.http;
        size_t count = 0, representation_count = 0;
        const auto query_start = req.target.find('?');
        if (query_start != std::string::npos) {
            // httplib collapses identical query pairs; the original target preserves them.
            std::istringstream query(req.target.substr(query_start + 1));
            std::string field;
            while (std::getline(query, field, '&')) {
                httplib::Params params;
                httplib::detail::parse_query_text(field, params);
                count += params.count("recipe");
                representation_count += params.count("representation");
            }
        }
        const auto representation = req.get_param_value("representation");
        if (representation_count > 1 || (representation_count && representation != "1" && representation != "2")) {
            res.status = 400;
            res.set_content(json{{"error", {{"message", "Expected one supported catalog representation"}, {"type", "invalid_request_error"}, {"code", "invalid_representation"}}}}.dump(), "application/json");
            return;
        }
        if (count > 1 || (count == 1 && req.get_param_value("recipe").empty())) {
            res.status = 400;
            res.set_content(json{{"error", {{"message", "Expected one nonempty recipe filter"}, {"type", "invalid_request_error"}, {"code", "invalid_recipe_filter"}}}}.dump(), "application/json");
            return;
        }
        const auto recipe = req.get_param_value("recipe");
        const auto schema = backends::frozen_backend_catalog().export_schema(recipe, representation == "2" ? 2 : 1);
        if (count && schema["recipes"].empty()) {
            res.status = 404;
            res.set_content(json{{"error", {{"message", "Unknown backend recipe: " + recipe}, {"type", "invalid_request_error"}, {"code", "unknown_recipe"}}}}.dump(), "application/json");
            return;
        }
        const auto etag = "\"" + backends::schema_digest(schema.dump()) + "\"";
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "private, no-cache");
        if (req.get_header_value("If-None-Match") == etag) { res.status = 304; return; }
        res.set_content(schema.dump(), "application/json");
    }
};

class BackendValidateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.backends_validate";
        s.methods = {"POST"};
        s.paths = {"backends/validate"};
        return s;
    }
    void handle(RouteRequest& request, httplib::Response& res) override {
        const auto& req = request.http;
        const auto result = backends::frozen_backend_catalog().validate(req.body);
        res.status = req.body.size() > 1024 * 1024 ? 413 : result["valid"].get<bool>() ? 200 : 400;
        res.set_content(result.dump(), "application/json");
    }
};

}

std::unique_ptr<ApiRoute> make_backend_schema_route(ServerContext& ctx) {
    return std::make_unique<BackendSchemaRoute>(ctx);
}

std::unique_ptr<ApiRoute> make_backend_validate_route(ServerContext& ctx) {
    return std::make_unique<BackendValidateRoute>(ctx);
}

}
