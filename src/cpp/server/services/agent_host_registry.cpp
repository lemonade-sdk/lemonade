#include "lemon/server/agent_host_registry.h"

#include <condition_variable>
#include <deque>
#include <vector>

#include "lemon/agents/host_protocol.h"
#include "lemon/utils/aixlog.hpp"

namespace lemon {

namespace protocol = agents::protocol;
using json = nlohmann::json;

struct AgentHostRegistry::ExchangeState {
    uint64_t id = 0;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<json> frames;
    bool finished = false;   // a response frame was queued
    bool cancelled = false;
};

struct AgentHostRegistry::Link {
    std::string token_hash;
    std::shared_ptr<AgentHostTransport> transport;
    std::mutex mutex;
    bool dead = false;
    std::map<uint64_t, std::weak_ptr<ExchangeState>> pending;

    bool send(const json& frame) {
        std::lock_guard<std::mutex> lock(mutex);
        return !dead && transport->send(protocol::serialize(frame));
    }
};

namespace {

void push_frame(AgentHostRegistry::ExchangeState& state, json frame) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.finished) {
            return;
        }
        state.finished = frame["type"] == "response";
        state.frames.push_back(std::move(frame));
    }
    state.cv.notify_all();
}

json unavailable_response(uint64_t id, const std::string& message) {
    return protocol::response(id, 503,
                              protocol::error_body("agent_host_unavailable", message));
}

// Sends and closes on the handler's WebSocket, which lives only while the
// handler runs; invalidate() runs before it returns.
class WsTransport : public AgentHostTransport {
public:
    explicit WsTransport(httplib::ws::WebSocket& ws) : ws_(&ws) {}

    bool send(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_ != nullptr && ws_->send(text);
    }

    void close(uint16_t code, const std::string& reason) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_ != nullptr) {
            ws_->close(static_cast<httplib::ws::CloseStatus>(code), reason);
        }
    }

    void invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_ = nullptr;
    }

private:
    std::mutex mutex_;
    httplib::ws::WebSocket* ws_;
};

} // namespace

AgentHostRegistry::Exchange::Exchange(AgentHostRegistry* registry, std::shared_ptr<Link> link,
                                      std::shared_ptr<ExchangeState> state)
    : registry_(registry), link_(std::move(link)), state_(std::move(state)) {}

AgentHostRegistry::Exchange::~Exchange() {
    bool finished;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        finished = state_->finished;
    }
    // An exchange dropped before its response (a timeout) still tells the host.
    if (!finished) {
        cancel();
    }
    {
        std::lock_guard<std::mutex> lock(link_->mutex);
        link_->pending.erase(state_->id);
    }
    registry_->relays_--;
}

std::optional<json> AgentHostRegistry::Exchange::next(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!state_->cv.wait_for(lock, timeout, [this] { return !state_->frames.empty(); })) {
        return std::nullopt;
    }
    json frame = std::move(state_->frames.front());
    state_->frames.pop_front();
    return frame;
}

void AgentHostRegistry::Exchange::cancel() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancelled || state_->finished) {
            return;
        }
        state_->cancelled = true;
    }
    link_->send(protocol::cancel(state_->id));
}

AgentHostRegistry::AgentHostRegistry(bool auth_required) : auth_required_(auth_required) {}

AgentHostRegistry::~AgentHostRegistry() {
    shutdown();
}

std::unique_ptr<AgentHostRegistry::Exchange> AgentHostRegistry::open(
    const std::string& token, const std::string& route, const json& params, const json& body,
    OpenError& error) {
    error = OpenError::None;
    std::shared_ptr<Link> link;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(protocol::token_hash(token));
        if (it == links_.end()) {
            error = OpenError::NoHost;
            return nullptr;
        }
        link = it->second;
        if (relays_.load() >= kMaxRelays) {
            error = OpenError::Busy;
            return nullptr;
        }
        relays_++;
    }

    auto state = std::make_shared<ExchangeState>();
    state->id = next_id_++;
    {
        std::lock_guard<std::mutex> lock(link->mutex);
        if (link->dead) {
            relays_--;
            error = OpenError::NoHost;
            return nullptr;
        }
        link->pending[state->id] = state;
    }
    auto exchange = std::make_unique<Exchange>(this, link, state);
    if (!link->send(protocol::request(state->id, route, params, token, body))) {
        push_frame(*state, unavailable_response(state->id, "The agent host disconnected."));
    }
    return exchange;
}

std::shared_ptr<AgentHostRegistry::Link> AgentHostRegistry::attach(
    const json& hello, std::shared_ptr<AgentHostTransport> transport) {
    if (hello.value("type", "") != "hello") {
        transport->close(protocol::kCloseBadHello, "expected hello");
        return nullptr;
    }
    if (hello.value("protocol", 0) != protocol::kVersion) {
        transport->close(protocol::kCloseProtocolMismatch, "unsupported protocol");
        return nullptr;
    }
    const std::string token = hello.value("token", "");
    if (!protocol::is_token(token)) {
        transport->close(protocol::kCloseBadHello, "invalid token");
        return nullptr;
    }

    auto link = std::make_shared<Link>();
    link->token_hash = protocol::token_hash(token);
    link->transport = std::move(transport);

    // welcome must reach the host before any request routed to it can.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || (links_.count(link->token_hash) == 0 && links_.size() >= kMaxLinks)) {
            link->transport->close(protocol::kCloseTooManyHosts, "too many agent hosts");
            return nullptr;
        }
    }
    link->send(protocol::welcome(auth_required_));

    std::shared_ptr<Link> replaced;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(link->token_hash);
        if (it != links_.end()) {
            replaced = it->second;
        } else if (stopping_ || links_.size() >= kMaxLinks) {
            link->transport->close(protocol::kCloseTooManyHosts, "too many agent hosts");
            return nullptr;
        }
        links_[link->token_hash] = link;
    }
    if (replaced) {
        fail_pending(*replaced, "The agent host reconnected.");
        replaced->transport->close(static_cast<uint16_t>(httplib::ws::CloseStatus::Normal),
                                   "replaced");
    }

    LOG(INFO, "AgentHost") << "Agent host connected (" << hello.value("version", "?") << ", "
                           << hello.value("platform", "?")
                           << (hello.value("persistent", false) ? ", persistent" : "") << ")"
                           << std::endl;
    return link;
}

void AgentHostRegistry::on_message(const std::shared_ptr<Link>& link, const std::string& text) {
    const auto frame = protocol::parse_frame(text);
    if (!frame || ((*frame)["type"] != "event" && (*frame)["type"] != "response")) {
        LOG(WARNING, "AgentHost") << "Ignoring a malformed frame from an agent host" << std::endl;
        return;
    }
    const uint64_t id = (*frame)["id"].get<uint64_t>();
    std::shared_ptr<ExchangeState> state;
    {
        std::lock_guard<std::mutex> lock(link->mutex);
        auto it = link->pending.find(id);
        if (it != link->pending.end()) {
            state = it->second.lock();
        }
    }
    if (state) {
        push_frame(*state, *frame);
    }
}

void AgentHostRegistry::detach(const std::shared_ptr<Link>& link) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(link->token_hash);
        if (it != links_.end() && it->second == link) {
            links_.erase(it);
            LOG(INFO, "AgentHost") << "Agent host disconnected" << std::endl;
        }
    }
    fail_pending(*link, "The agent host disconnected.");
}

void AgentHostRegistry::fail_pending(Link& link, const std::string& message) {
    std::vector<std::shared_ptr<ExchangeState>> states;
    {
        std::lock_guard<std::mutex> lock(link.mutex);
        link.dead = true;
        for (auto& [id, weak] : link.pending) {
            if (auto state = weak.lock()) {
                states.push_back(state);
            }
        }
    }
    for (auto& state : states) {
        push_frame(*state, unavailable_response(state->id, message));
    }
}

void AgentHostRegistry::shutdown() {
    stopping_ = true;
    std::map<std::string, std::shared_ptr<Link>> links;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        links.swap(links_);
    }
    for (auto& [hash, link] : links) {
        fail_pending(*link, "Lemonade is shutting down.");
    }
}

size_t AgentHostRegistry::link_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return links_.size();
}

void AgentHostRegistry::register_endpoint(httplib::Server& server) {
    // Called for the new servers of a rebind, after the old ones have stopped.
    stopping_ = false;
    server.set_websocket_ping_interval(kPingIntervalSeconds);
    server.set_websocket_max_missed_pongs(2);
    server.WebSocket(protocol::kEndpoint,
                     [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
        auto transport = std::make_shared<WsTransport>(ws);
        // A bounded read lets this worker notice the hello deadline and
        // shutdown() without another thread having to wake read().
        ws.set_read_timeout(1, 0);
        const auto deadline = std::chrono::steady_clock::now() + kHelloTimeout;
        std::shared_ptr<Link> link;
        std::string message;
        while (!stopping_) {
            const auto result = ws.read(message);
            if (result == httplib::ws::Timeout) {
                if (!link && std::chrono::steady_clock::now() >= deadline) {
                    ws.close(static_cast<httplib::ws::CloseStatus>(protocol::kCloseHelloTimeout),
                             "no hello");
                    break;
                }
                continue;
            }
            if (result != httplib::ws::Text) {
                break;
            }
            if (!link) {
                const auto frame = protocol::parse_frame(message);
                link = frame ? attach(*frame, transport) : nullptr;
                if (!frame) {
                    ws.close(static_cast<httplib::ws::CloseStatus>(protocol::kCloseBadHello),
                             "expected hello");
                }
                if (!link) {
                    break;
                }
                continue;
            }
            on_message(link, message);
        }
        // On shutdown the handler returns without a close handshake, which
        // would wait on the host's reply; the host sees the drop and reconnects.
        if (link) {
            detach(link);
        }
        transport->invalidate();
    });
}

} // namespace lemon
