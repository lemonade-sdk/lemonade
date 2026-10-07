#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class DeleteRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.delete";
        s.methods = {"DELETE"};
        s.paths = {"/api/delete"};
        s.prefixes = Prefixes::Root;
        s.summary = "Delete a model";
        s.description =
            "Deletes a downloaded model's files, unloading it first when it is loaded.";
        s.notes = {
            "Success answers `200` with an empty body, as Ollama does. An unknown or "
            "unsupported model answers `404`.",
        };
        s.args = {
            {"model", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Model to delete. A `:latest` tag is ignored."},
            {"name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "Older name for `model`; wins when both are given."},
        };

        // Deleting a model the docs need would break later examples, so this deletes the
        // one its own setup pulls.
        RouteResponse response;
        response.format = ResponseFormat::Empty;
        response.setup = {{"ollama.pull", ResponseFormat::Json}};
        response.example = json::parse(R"({"model": "RealESRGAN-x4plus-anime"})");
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            auto request_json = json::parse(req.http.body);
            std::string name = strip_latest_tag(request_json.value("name", request_json.value("model", "")));

            if (name.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"name is required"})", "application/json");
                return;
            }

            if (ctx_.router->is_model_loaded(name)) {
                ctx_.router->unload_model(name);
            }

            ctx_.model_manager->delete_model(name);

            res.status = 200;

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/delete: " << e.what() << std::endl;
            std::string error_msg = e.what();
            if (error_msg.find("not found") != std::string::npos ||
                error_msg.find("not supported") != std::string::npos) {
                res.status = 404;
            } else {
                res.status = 500;
            }
            json error = {{"error", std::string(e.what())}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_delete_route(ServerContext& ctx) {
    return std::make_unique<DeleteRoute>(ctx);
}

} // namespace lemon
