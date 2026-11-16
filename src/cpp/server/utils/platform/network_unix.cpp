#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/network_utils.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#define LEMON_TCP_KEEPIDLE TCP_KEEPALIVE
#elif defined(TCP_KEEPIDLE)
#define LEMON_TCP_KEEPIDLE TCP_KEEPIDLE
#endif

namespace lemon::utils {

namespace {

std::mutex refused_mutex;
std::set<std::string> refused_addresses;
std::atomic<bool> any_refused{false};

} // namespace

void set_refused_local_addresses(const std::vector<std::string>& addresses) {
    std::lock_guard<std::mutex> lock(refused_mutex);
    refused_addresses = std::set<std::string>(addresses.begin(), addresses.end());
    any_refused.store(!refused_addresses.empty());
}

bool arrived_on_refused_address(socket_t sock) {
    if (!any_refused.load()) return false;
    sockaddr_storage local = {};
    socklen_t length = sizeof(local);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) != 0) return false;
    char address[INET6_ADDRSTRLEN] = {};
    if (local.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&local)->sin_addr, address, sizeof(address));
    } else if (local.ss_family == AF_INET6) {
        const in6_addr* v6 = &reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr;
        // The "::" listener takes IPv4 connections as v4-mapped addresses.
        if (IN6_IS_ADDR_V4MAPPED(v6)) {
            inet_ntop(AF_INET, &v6->s6_addr[12], address, sizeof(address));
        } else {
            inet_ntop(AF_INET6, v6, address, sizeof(address));
        }
    } else {
        return false;
    }
    std::lock_guard<std::mutex> lock(refused_mutex);
    return refused_addresses.count(address) > 0;
}

bool configure_tcp_keepalive(socket_t sock) {
    int opt = 1;
    bool ok = true;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) != 0) {
        LOG(DEBUG, "Server") << "setsockopt(SO_KEEPALIVE) failed: " << std::strerror(errno) << " (" << errno << ")" << std::endl;
        ok = false;
    }
#ifdef LEMON_TCP_KEEPIDLE
    int idle = 15;
    if (setsockopt(sock, IPPROTO_TCP, LEMON_TCP_KEEPIDLE, &idle, sizeof(idle)) != 0) {
        LOG(DEBUG, "Server") << "setsockopt(LEMON_TCP_KEEPIDLE) failed: " << std::strerror(errno) << " (" << errno << ")" << std::endl;
        ok = false;
    }
#endif
#ifdef TCP_KEEPINTVL
    int interval = 5;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0) {
        LOG(DEBUG, "Server") << "setsockopt(TCP_KEEPINTVL) failed: " << std::strerror(errno) << " (" << errno << ")" << std::endl;
        ok = false;
    }
#endif
#ifdef TCP_KEEPCNT
    int count = 3;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
        LOG(DEBUG, "Server") << "setsockopt(TCP_KEEPCNT) failed: " << std::strerror(errno) << " (" << errno << ")" << std::endl;
        ok = false;
    }
#endif
    return ok;
}

bool is_tcp_listener_active(int family, const std::string& host_ip, int port) {
    int sock = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }
    auto close_sock = [&]() {
        close(sock);
    };

    std::string connect_ip = host_ip;
    if (connect_ip == "0.0.0.0") {
        connect_ip = "127.0.0.1";
    } else if (connect_ip == "::") {
        connect_ip = "::1";
    }

    sockaddr_storage ss{};
    socklen_t len;
    if (family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        a->sin_family = AF_INET;
        a->sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, connect_ip.c_str(), &a->sin_addr) != 1) {
            close_sock();
            return false;
        }
        len = sizeof(sockaddr_in);
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET6, connect_ip.c_str(), &a->sin6_addr) != 1) {
            close_sock();
            return false;
        }
        len = sizeof(sockaddr_in6);
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        close_sock();
        return false;
    }

    int res = connect(sock, reinterpret_cast<sockaddr*>(&ss), len);
    bool has_listener = false;

    if (res == 0) {
        has_listener = true;
    } else {
        int err = errno;
        bool in_progress = (err == EINPROGRESS || err == EALREADY);
        if (in_progress) {
            if (sock >= FD_SETSIZE) {
                close_sock();
                return false;
            }

            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100000;

            int sel = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
            if (sel > 0 && FD_ISSET(sock, &write_fds)) {
                int socket_error = 0;
                socklen_t opt_len = sizeof(socket_error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &opt_len) == 0) {
                    if (socket_error == 0) {
                        has_listener = true;
                    }
                }
            }
        }
    }

    close_sock();
    return has_listener;
}

} // namespace lemon::utils
