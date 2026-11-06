// lemond's side of the agent host link: routing by token, caps, disconnects
// and shutdown. Most cases drive the registry directly; the last ones use a
// real WebSocket on loopback.
#include "agent_test_util.h"

#include <atomic>
#include <thread>
#include <vector>

#include "lemon/agents/host_protocol.h"
#include "lemon/server/agent_host_registry.h"

using agent_test::check;
using lemon::AgentHostRegistry;
using nlohmann::json;
namespace protocol = lemon::agents::protocol;
using namespace std::chrono_literals;

namespace {

struct FakeTransport : lemon::AgentHostTransport {
    std::mutex mutex;
    std::vector<json> sent;
    int close_code = 0;
    bool send(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (close_code != 0) return false;
        sent.push_back(json::parse(text));
        return true;
    }
    void close(uint16_t code, const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex);
        close_code = code;
    }
    json last() {
        std::lock_guard<std::mutex> lock(mutex);
        return sent.empty() ? json() : sent.back();
    }
};

std::string token_of(char c) {
    return std::string(64, c);
}

json hello(const std::string& token) {
    return protocol::hello(token, "test", "linux-x64", false);
}

} // namespace

int main() {
    AgentHostRegistry registry(true);
    AgentHostRegistry::OpenError error;

    auto t1 = std::make_shared<FakeTransport>();
    auto link1 = registry.attach(hello(token_of('a')), t1);
    check("hello attaches", link1 != nullptr && registry.link_count() == 1);
    check("welcome carries auth_required",
          t1->last()["type"] == "welcome" && t1->last()["auth_required"] == true);

    check("unknown token finds no host",
          registry.open(token_of('b'), "r", json::object(), {}, error) == nullptr &&
              error == AgentHostRegistry::OpenError::NoHost);

    auto exchange = registry.open(token_of('a'), "lemonade.agents_list", json::object(), {}, error);
    check("known token opens an exchange", exchange != nullptr);
    const json request = t1->last();
    check("request frame carries route and token",
          request["type"] == "request" && request["route"] == "lemonade.agents_list" &&
              request["token"] == token_of('a'));
    const uint64_t id = request["id"];
    registry.on_message(link1, protocol::event(id, "status", {{"message", "hi"}}).dump());
    registry.on_message(link1, protocol::response(id, 200, {{"ok", true}}).dump());
    auto first = exchange->next(100ms);
    auto second = exchange->next(100ms);
    check("event then response are relayed in order",
          first && (*first)["type"] == "event" && second && (*second)["status"] == 200);
    exchange.reset();
    check("a finished exchange frees its relay", registry.relay_count() == 0);

    // Relay cap.
    std::vector<std::unique_ptr<AgentHostRegistry::Exchange>> held;
    for (size_t i = 0; i < AgentHostRegistry::kMaxRelays; ++i) {
        held.push_back(registry.open(token_of('a'), "r", json::object(), {}, error));
    }
    check("relay cap", registry.open(token_of('a'), "r", json::object(), {}, error) == nullptr &&
                           error == AgentHostRegistry::OpenError::Busy);
    const uint64_t held_id = t1->last()["id"];
    held.pop_back();
    check("dropping an unanswered exchange cancels it",
          t1->last()["type"] == "cancel" && t1->last()["id"] == held_id);

    // Disconnect fails pending requests with 503.
    registry.detach(link1);
    auto failed = held.front()->next(100ms);
    check("disconnect fails pending with 503",
          failed && (*failed)["type"] == "response" && (*failed)["status"] == 503);
    held.clear();
    check("detached host is gone", registry.link_count() == 0);

    // Reconnect replaces the link.
    auto t2 = std::make_shared<FakeTransport>();
    auto link2 = registry.attach(hello(token_of('a')), t2);
    auto pending = registry.open(token_of('a'), "r", json::object(), {}, error);
    auto t3 = std::make_shared<FakeTransport>();
    auto link3 = registry.attach(hello(token_of('a')), t3);
    auto replaced = pending->next(100ms);
    check("reconnect replaces the link", registry.link_count() == 1 && t2->close_code == 1000);
    check("reconnect fails the old link's pending", replaced && (*replaced)["status"] == 503);
    pending.reset();
    registry.detach(link2);
    check("the old link's detach keeps the new one", registry.link_count() == 1);

    // Link cap and bad hellos.
    std::vector<std::shared_ptr<FakeTransport>> transports;
    for (char c : {'b', 'c', 'd'}) {
        transports.push_back(std::make_shared<FakeTransport>());
        registry.attach(hello(token_of(c)), transports.back());
    }
    auto t5 = std::make_shared<FakeTransport>();
    check("link cap closes with 4429", registry.attach(hello(token_of('e')), t5) == nullptr &&
                                           t5->close_code == protocol::kCloseTooManyHosts);
    auto t6 = std::make_shared<FakeTransport>();
    json old = hello(token_of('f'));
    old["protocol"] = 2;
    check("protocol mismatch closes with 4409",
          registry.attach(old, t6) == nullptr && t6->close_code == protocol::kCloseProtocolMismatch);
    auto t7 = std::make_shared<FakeTransport>();
    check("bad token closes with 4400", registry.attach(hello("short"), t7) == nullptr &&
                                            t7->close_code == protocol::kCloseBadHello);

    auto in_flight = registry.open(token_of('a'), "r", json::object(), {}, error);
    registry.shutdown();
    auto shut = in_flight->next(100ms);
    check("shutdown fails pending exchanges", shut && (*shut)["status"] == 503);
    check("shutdown drops every link", registry.link_count() == 0);
    in_flight.reset();

    // Real sockets: a silent client is closed with 4408, and shutdown()
    // frees a connected handler promptly.
    AgentHostRegistry live(false);
    httplib::Server server;
    live.register_endpoint(server);
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();
    const std::string url = "ws://127.0.0.1:" + std::to_string(port) + protocol::kEndpoint;

    {
        httplib::ws::WebSocketClient silent(url);
        const auto start = std::chrono::steady_clock::now();
        check("silent client connects", static_cast<bool>(silent.connect()));
        silent.set_read_timeout(10, 0);
        std::string msg;
        while (silent.read(msg) == httplib::ws::Text) {
        }
        const auto waited = std::chrono::steady_clock::now() - start;
        check("silent client is closed within 7 s", waited < 7s && waited >= 4s);
    }
    {
        httplib::ws::WebSocketClient host(url);
        check("host connects", static_cast<bool>(host.connect()));
        host.send(protocol::hello(token_of('9'), "test", "linux-x64", false).dump());
        std::string msg;
        host.set_read_timeout(5, 0);
        const auto read_result = host.read(msg);
        check("host gets welcome", read_result == httplib::ws::Text &&
                                       json::parse(msg)["auth_required"] == false);
        const auto start = std::chrono::steady_clock::now();
        live.shutdown();
        server.stop();
        server_thread.join();
        check("shutdown with a connected host returns within 2 s",
              std::chrono::steady_clock::now() - start < 2s);
    }
    return agent_test::finish();
}
