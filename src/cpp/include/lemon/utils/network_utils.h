#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
#else
using socket_t = int;
#endif

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

// Enable TCP keepalive probes with 15s initial idle, 5s retry interval, and
// 3 probe attempts (~30s total dead-peer detection) to prevent stateful L4
// firewalls, NAT gateways, and intermediate routers from silently dropping
// idle TCP connections during prolonged inactivity (e.g. prompt evaluation).
// Note: application-layer idle timeouts (e.g. nginx proxy_read_timeout or ALB
// idle timeout) require periodic application data / SSE heartbeats.
bool configure_tcp_keepalive(socket_t sock);

} // namespace lemon::utils
