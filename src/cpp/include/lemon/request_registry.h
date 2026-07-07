#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lemon {

std::string generate_request_id();

struct ActiveRequest {
    std::string request_id;
    std::string model_name;
    std::string endpoint;
    bool is_streaming = false;
    std::chrono::steady_clock::time_point start_time;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    void* server = nullptr;
};

class RequestRegistry;

class ActiveRequestGuard {
public:
    ActiveRequestGuard() = default;
    ~ActiveRequestGuard();

    ActiveRequestGuard(const ActiveRequestGuard&) = delete;
    ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;

    ActiveRequestGuard(ActiveRequestGuard&& other) noexcept;
    ActiveRequestGuard& operator=(ActiveRequestGuard&& other) noexcept;

    const std::string& request_id() const { return request_id_; }
    std::shared_ptr<std::atomic<bool>> cancel_flag() const { return cancel_flag_; }
    bool valid() const { return !request_id_.empty(); }

private:
    friend class RequestRegistry;
    ActiveRequestGuard(RequestRegistry* registry,
                       const std::string& request_id,
                       std::shared_ptr<std::atomic<bool>> cancel_flag);

    RequestRegistry* registry_ = nullptr;
    std::string request_id_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_;
};

class RequestRegistry {
public:
    RequestRegistry() = default;

    ActiveRequestGuard register_request(const std::string& request_id,
                                        const std::string& model_name,
                                        const std::string& endpoint,
                                        bool is_streaming,
                                        void* server);

    void unregister_request(const std::string& request_id);

    bool cancel_request(const std::string& request_id);

    std::vector<ActiveRequest> list_active_requests() const;

    int cancel_all();

private:
    mutable std::mutex mutex_;
    std::map<std::string, ActiveRequest> requests_;
};

} // namespace lemon
