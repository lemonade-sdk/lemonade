#include <chrono>
#include <cstdlib>
#include <thread>

#include <lemon/utils/aixlog.hpp>

#include "lemon/router.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/download_manager.h"

namespace lemon {
namespace {

using json = nlohmann::json;

class ShutdownRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.shutdown";
        s.methods = {"POST"};
        s.paths = {"shutdown"};
        s.prefixes = Prefixes::Internal;
        s.summary = "Unload every model and stop lemond";
        s.description = "Unloads every model, answers, and then stops lemond.";
        s.notes = {
            "Models unload before the response is sent, so backend processes such as "
            "`llama-server` have exited by the time the caller reads it. lemond cancels its "
            "downloads and exits about 100 ms later.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["status"],
            "properties": {
                "status": {"const": "shutting down"}
            }
        })");
        response.example = json::object();
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        LOG(INFO, "Server") << "Shutdown request received" << std::endl;

        // Child processes (llama-server, etc.) must be gone before the caller proceeds,
        // or they outlive lemond as zombies.
        LOG(INFO, "Server") << "Unloading models and stopping backend servers..." << std::endl;
        try {
            ctx_.router->unload_model();
            LOG(INFO, "Server") << "All models unloaded" << std::endl;
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "Error during unload: " << e.what() << std::endl;
        }

        res.set_content(json{{"status", "shutting down"}}.dump(), "application/json");

        // Stopping from this thread would cut off the response, so stop shortly after it.
        ServerContext* ctx = &ctx_;
        std::thread([ctx]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ctx->downloads->cancel_all();
            ctx->server->stop();
            std::exit(0);
        }).detach();
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_shutdown_route(ServerContext& ctx) {
    return std::make_unique<ShutdownRoute>(ctx);
}

} // namespace lemon
