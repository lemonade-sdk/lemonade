#include "lemon/agents/host_link.h"

#include <memory>
#include <mutex>
#include <thread>

#include <httplib.h>

#include "lemon/agents/agent_lock.h"
#include "lemon/agents/agent_pins.h"
#include "lemon/agents/host_protocol.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/version.h"

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace lemon::agents {

namespace {

// Request threads outlive a dropped connection; their late frames go nowhere.
class Outbox {
public:
    explicit Outbox(httplib::ws::WebSocketClient* ws) : ws_(ws) {}
    void send(const json& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_ != nullptr) {
            ws_->send(frame.dump());
        }
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_ = nullptr;
    }

private:
    std::mutex mutex_;
    httplib::ws::WebSocketClient* ws_;
};

// Another host waiting to take over holds takeover.lock.
bool takeover_requested(const HostContext& ctx) {
    return FileLock::try_acquire(ctx.paths.takeover_lock()) == nullptr;
}

} // namespace

LinkExit run_link(HostContext& ctx, HostRoutes& routes, const LinkOptions& options) {
    auto backoff = std::chrono::seconds(1);
    auto last_connected = Clock::now();
    auto last_busy = Clock::now();

    while (true) {
        httplib::ws::WebSocketClient ws(options.ws_url);
        if (!ctx.api_key.empty()) {
            ws.set_bearer_token_auth(ctx.api_key);
        }
        ws.set_connection_timeout(3, 0);
        ws.set_websocket_ping_interval(15);
        ws.set_websocket_max_missed_pongs(2);
        const auto result = ws.connect();
        if (!result) {
            if (result.status() == 401 || result.status() == 403) {
                LOG(ERROR, "AgentHost") << "lemond rejected the API key (HTTP " << result.status()
                                        << ")" << std::endl;
                if (!options.persistent) {
                    return LinkExit::Unauthorized;
                }
            } else {
                LOG(DEBUG, "AgentHost") << "Can't connect to " << options.ws_url << std::endl;
            }
            if (!options.persistent && Clock::now() - last_connected > options.unreachable_exit) {
                return LinkExit::Unreachable;
            }
            if (!options.persistent && routes.in_flight() == 0 && takeover_requested(ctx)) {
                return LinkExit::TakenOver;
            }
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, options.max_backoff);
            continue;
        }

        ws.set_read_timeout(1, 0);
        ws.send(protocol::hello(ctx.token, LEMON_VERSION_STRING, host_os() + "-" + host_arch(),
                                options.persistent)
                    .dump());
        auto outbox = std::make_shared<Outbox>(&ws);
        bool welcomed = false;
        const auto connected_at = Clock::now();
        std::string message;
        while (true) {
            const auto read = ws.read(message);
            const auto now = Clock::now();
            if (read == httplib::ws::Timeout) {
                if (!welcomed && now - connected_at > std::chrono::seconds(10)) {
                    break;
                }
                if (routes.in_flight() > 0) {
                    last_busy = now;
                } else if (!options.persistent) {
                    if (takeover_requested(ctx)) {
                        outbox->close();
                        ws.close();
                        return LinkExit::TakenOver;
                    }
                    if (now - last_busy > options.idle_exit) {
                        outbox->close();
                        ws.close();
                        return LinkExit::Idle;
                    }
                }
                continue;
            }
            if (read != httplib::ws::Text) {
                break;
            }
            const auto frame = protocol::parse_frame(message);
            if (!frame) {
                continue;
            }
            const std::string type = (*frame)["type"];
            if (type == "welcome") {
                welcomed = true;
                backoff = std::chrono::seconds(1);
                ctx.auth_required = (*frame)["auth_required"].get<bool>();
                LOG(INFO, "AgentHost") << "Connected to " << options.ws_url << std::endl;
            } else if (type == "request") {
                last_busy = now;
                std::thread([&routes, outbox, request = *frame] {
                    Responder responder(request["id"].get<uint64_t>(),
                                        [outbox](const json& out) { outbox->send(out); });
                    routes.handle(request, responder);
                }).detach();
            } else if (type == "cancel") {
                routes.cancel((*frame)["id"].get<uint64_t>());
            }
        }
        outbox->close();
        last_connected = Clock::now();
        LOG(INFO, "AgentHost") << "Disconnected from lemond; reconnecting" << std::endl;
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, options.max_backoff);
    }
}

} // namespace lemon::agents
