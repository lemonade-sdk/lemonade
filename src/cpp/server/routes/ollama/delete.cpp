#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/api_route.h"
#include "lemon/server/gateway_conversion.h"
#include "lemon/server/model_registration.h"
#include "lemon/server/shared_args.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Success answers `200` with an empty body, as Ollama does.",
        };
        s.args = {ollama::management::kModel};

        // Deleting a model the docs need would break later examples, so this deletes the
        // one its own setup pulls.
        RouteResponse response;
        response.format = ResponseFormat::Empty;
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Empty;
        example.setup = {{"ollama.pull", "non-streaming"}};
        example.args = ordered_json::parse(R"({"model": "RealESRGAN-x4plus-anime"})");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kOllamaModelNotFound, kDeleteFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string name = strip_latest_tag(req.arg(ollama::management::kModel).get<std::string>());

            delete_model_files(ctx_, name);

            res.status = 200;

        } catch (const std::exception& e) {
            LOG(ERROR, "OllamaApi") << "Error in /api/delete: " << e.what() << std::endl;
            write_delete_error(e, kOllamaModelNotFound, res);
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_delete_route(ServerContext& ctx) {
    return std::make_unique<DeleteRoute>(ctx);
}

} // namespace lemon
