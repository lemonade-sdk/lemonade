#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace lemon {

// One connected agent host as the registry sees it: something frames can be
// sent to and that can be closed. The WebSocket handler provides the real one.
class AgentHostTransport {
public:
    virtual ~AgentHostTransport() = default;
    virtual bool send(const std::string& text) = 0;
    virtual void close(uint16_t code, const std::string& reason) = 0;
};

// Routes /v1/agents/* requests to the agent host whose token the client sent.
// Hosts dial in over the WebSocket at agents::protocol::kEndpoint.
class AgentHostRegistry {
public:
    // Every link and relay holds a pool worker while it lasts, and the pool
    // can be as small as 32, so agents get at most a quarter of it.
    static constexpr size_t kMaxLinks = 4;
    static constexpr size_t kMaxRelays = 4;
    static constexpr std::chrono::seconds kHelloTimeout{5};

    struct Link;
    struct ExchangeState;

    class Exchange {
    public:
        Exchange(AgentHostRegistry* registry, std::shared_ptr<Link> link,
                 std::shared_ptr<ExchangeState> state);
        ~Exchange();
        Exchange(const Exchange&) = delete;
        Exchange& operator=(const Exchange&) = delete;

        // The next event or response frame, or nullopt on timeout. When the
        // host goes away the exchange ends with a 503 response frame.
        std::optional<nlohmann::json> next(std::chrono::milliseconds timeout);
        // Tells the host the client is gone. Idempotent.
        void cancel();

    private:
        AgentHostRegistry* registry_;
        std::shared_ptr<Link> link_;
        std::shared_ptr<ExchangeState> state_;
    };

    enum class OpenError { None, NoHost, Busy };

    explicit AgentHostRegistry(bool auth_required);
    ~AgentHostRegistry();

    // nullptr with `error` set when no host has the token or the relay cap is reached.
    std::unique_ptr<Exchange> open(const std::string& token, const std::string& route,
                                   const nlohmann::json& params, const nlohmann::json& body,
                                   OpenError& error);

    // The link lifecycle. The WebSocket handler drives it; tests drive it directly.
    // attach() answers a valid hello with welcome, or closes the transport and returns nullptr.
    std::shared_ptr<Link> attach(const nlohmann::json& hello,
                                 std::shared_ptr<AgentHostTransport> transport);
    void on_message(const std::shared_ptr<Link>& link, const std::string& text);
    void detach(const std::shared_ptr<Link>& link);

    void register_endpoint(httplib::Server& server);

    // Fails every pending exchange and makes every current link handler
    // return within about a second. Must run before the listener stops, since
    // stopping joins the workers the handlers run on. Hosts that connect
    // afterwards (after a rebind) are served normally.
    void shutdown();

    size_t link_count() const;
    size_t relay_count() const { return relays_.load(); }

private:
    void fail_pending(Link& link, const std::string& message);

    const bool auth_required_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<size_t> relays_{0};
    std::atomic<uint64_t> next_id_{1};
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Link>> links_;  // by token hash
};

} // namespace lemon
