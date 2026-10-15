#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kNotLoaded{
    404, "",
    "The named model is not loaded, which includes an omni collection, whose components are "
    "unloaded by their own names"};
const RouteError kUnloadFailed{500, "", "Unloading fails for another reason"};

class UnloadRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.unload";
        s.methods = {"POST"};
        s.paths = {"unload"};
        s.summary = "Unload a model";
        s.description =
            "Unloads a model from memory, freeing it while `lemond` keeps running.";
        s.notes = {
            "An unparsable body also unloads every model.",
        };
        s.args = {
            {"model_name", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "The model to unload; omit it to unload every model. `model` is accepted as an alias."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["status", "message"],
            "properties": {
                "status": {"const": "success", "description": "Always `success`."},
                "message": {"type": "string", "description": "A confirmation that one model, or every model, was unloaded."},
                "model_name": {"type": "string", "description": "Only when one model was unloaded."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.setup = {{"lemonade.load", "model"}};
        example.args = {{"model_name", "Qwen3-0.6B-GGUF"}};
        s.examples = {example};
        s.errors = {kNotLoaded, kUnloadFailed};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const httplib::Request& http = req.http;
        try {
            LOG(INFO, "Server") << "Unload request received" << std::endl;
            LOG(DEBUG, "Server") << "Request method: " << http.method << ", body length: " << http.body.length() << std::endl;
            LOG(DEBUG, "Server") << "Content-Type: " << http.get_header_value("Content-Type") << std::endl;

            std::string model_name;
            if (!http.body.empty()) {
                try {
                    auto request_json = json::parse(http.body);
                    resolve_model_name(ctx_, request_json);
                    if (request_json.contains("model_name") && request_json["model_name"].is_string()) {
                        model_name = request_json["model_name"].get<std::string>();
                    } else if (request_json.contains("model") && request_json["model"].is_string()) {
                        model_name = request_json["model"].get<std::string>();
                    }
                } catch (...) {
                    // An unparsable body unloads every model.
                }
            }

            ctx_.router->unload_model(model_name);  // Empty string = unload all

            if (model_name.empty()) {
                LOG(INFO, "Server") << "All models unloaded successfully" << std::endl;
                json response = {
                    {"status", "success"},
                    {"message", "All models unloaded successfully"}
                };
                res.status = 200;
                res.set_content(response.dump(), "application/json");
            } else {
                LOG(INFO, "Server") << "Model '" << model_name << "' unloaded successfully" << std::endl;
                json response = {
                    {"status", "success"},
                    {"message", "Model unloaded successfully"},
                    {"model_name", model_name}
                };
                res.status = 200;
                res.set_content(response.dump(), "application/json");
            }
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Unload failed: " << e.what() << std::endl;

            std::string error_msg = e.what();
            res.status = error_msg.find("not loaded") != std::string::npos ? kNotLoaded.status
                                                                         : kUnloadFailed.status;
            json error = {{"error", e.what()}};
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_unload_route(ServerContext& ctx) {
    return std::make_unique<UnloadRoute>(ctx);
}

} // namespace lemon
