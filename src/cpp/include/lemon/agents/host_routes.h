#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/agents/pi_driver.h"

namespace lemon::agents {

// Sends frames for one request back to lemond.
class Responder {
public:
    using SendFn = std::function<void(const nlohmann::json& frame)>;
    Responder(uint64_t id, SendFn send) : id_(id), send_(std::move(send)) {}

    void event(const std::string& name, const nlohmann::json& data);
    void respond(int status, const nlohmann::json& body);
    bool responded() const { return responded_; }

private:
    uint64_t id_;
    SendFn send_;
    bool responded_ = false;
};

class HostRoutes {
public:
    explicit HostRoutes(HostContext& ctx) : ctx_(ctx) {}

    // Runs one request frame to completion on the calling thread.
    void handle(const nlohmann::json& request, Responder& responder);
    void cancel(uint64_t id);
    size_t in_flight() const { return in_flight_.load(); }

private:
    nlohmann::json list();
    nlohmann::json install(const std::string& id, const nlohmann::json& body, Responder& responder,
                           const std::shared_ptr<std::atomic<bool>>& cancelled);
    nlohmann::json uninstall(const std::string& id);
    nlohmann::json configure(const std::string& id, const nlohmann::json& body);
    nlohmann::json launch(const std::string& id, const nlohmann::json& body, Responder& responder);

    // Marks an agent as being installed or removed for the guard's lifetime.
    class Installing {
    public:
        Installing(HostRoutes& routes, const std::string& id);
        ~Installing();

    private:
        HostRoutes& routes_;
        std::string id_;
    };
    bool is_installing(const std::string& id);

    HostContext& ctx_;
    std::atomic<size_t> in_flight_{0};
    std::mutex mutex_;
    std::set<std::string> installing_;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancels_;
};

} // namespace lemon::agents
