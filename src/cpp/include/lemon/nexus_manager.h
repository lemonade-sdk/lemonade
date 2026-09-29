#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

namespace lemon {
class NexusManager {
public:
    NexusManager(const std::string& config_dir, int server_port);
    ~NexusManager();
    void register_routes(httplib::Server& server, bool management = false);
    void stop();
    nlohmann::json status() const;
    bool enable(std::string& error);
    void disable();
private:
    using json = nlohmann::json;
    struct Session;
    void supervise();
    void persist_enabled_locked();
    void update_snapshot_locked(const json& snapshot);
    void emit_locked(const std::string& type, const json& data);
    void handle(const httplib::Request& req, httplib::Response& res, const std::string& operation);
    std::pair<int, json> control_locked(const std::string& method, const std::string& path, const json& body);
    std::string data_root_;
    int server_port_;
    std::unique_ptr<Session> session_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool enabled_{false};
    bool stopping_{false};
    json snapshot_ = {{"status", "disabled"}, {"locked", true}, {"mesh_up", false}, {"members", json::array()}};
    json active_invite_ = nullptr;
    uint64_t event_sequence_{0};
    std::deque<json> events_;
    std::thread worker_;
    httplib::Server management_server_;
    std::thread management_thread_;
    int management_port_{0};
};
}
