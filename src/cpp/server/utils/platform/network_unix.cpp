#include "lemon/utils/network_utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace lemon::utils {

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
