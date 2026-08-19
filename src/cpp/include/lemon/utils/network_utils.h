#pragma once

#include <atomic>
#include <cstddef>
#include <string>

namespace lemon::utils {

bool is_tcp_listener_active(int family, const std::string& host_ip, int port);

class ListenerStartupState {
public:
    explicit ListenerStartupState(std::size_t expected_bind_attempts)
        : expected_bind_attempts_(expected_bind_attempts) {}

    void record_bind_success() {
        completed_bind_attempts_.fetch_add(1);
    }

    void record_bind_failure() {
        failed_.store(true);
        completed_bind_attempts_.fetch_add(1);
    }

    void record_listen_failure() {
        failed_.store(true);
    }

    bool bind_attempts_complete() const {
        return completed_bind_attempts_.load() == expected_bind_attempts_;
    }

    bool failed() const {
        return failed_.load();
    }

private:
    const std::size_t expected_bind_attempts_;
    std::atomic<std::size_t> completed_bind_attempts_{0};
    std::atomic<bool> failed_{false};
};

} // namespace lemon::utils
