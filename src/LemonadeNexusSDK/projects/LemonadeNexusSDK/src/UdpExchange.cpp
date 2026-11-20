#include "UdpExchange.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

namespace lnsdk::udp_exchange {

namespace {

/// The largest PCP message (RFC 6887 section 7); NAT-PMP messages are
/// smaller.
constexpr size_t kMaxDatagram = 1100;

Result<std::vector<uint8_t>> fail(const char* protocol, const char* what) {
    Result<std::vector<uint8_t>> r;
    r.error = std::string(protocol) + " " + what;
    return r;
}

void set_receive_timeout(socket_t sock, std::chrono::milliseconds wait) {
    const auto ms = std::max<long long>(1, wait.count());
#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(ms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool receive_timed_out() {
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

socket_t open_socket() {
#ifdef _WIN32
    static std::once_flag wsa_once;
    std::call_once(wsa_once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) return kInvalidSocket;
#ifdef __APPLE__
    int no_sigpipe = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    return sock;
}

}  // namespace

Result<std::vector<uint8_t>> exchange(const std::string& gateway_ipv4, uint16_t port,
                                      const std::optional<std::string>& local_ipv4,
                                      std::span<const uint8_t> request,
                                      std::span<const std::chrono::milliseconds> waits,
                                      const Acceptor& accept, const char* protocol,
                                      const std::atomic<bool>* cancel) {
    using Reply = Result<std::vector<uint8_t>>;
    const auto cancelled = [cancel] { return cancel && cancel->load(); };

    sockaddr_in gateway{};
    gateway.sin_family = AF_INET;
    gateway.sin_port = htons(port);
    if (inet_pton(AF_INET, gateway_ipv4.c_str(), &gateway.sin_addr) != 1) {
        return fail(protocol, "gateway address is not a dotted IPv4 address");
    }
    sockaddr_in local{};
    if (local_ipv4) {
        local.sin_family = AF_INET;
        if (inet_pton(AF_INET, local_ipv4->c_str(), &local.sin_addr) != 1) {
            return fail(protocol, "local address is not a dotted IPv4 address");
        }
    }

    socket_t sock = open_socket();
    if (sock == kInvalidSocket) return fail(protocol, "UDP socket failed");
    if (local_ipv4 && bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        close_socket(sock);
        return fail(protocol, "UDP socket could not bind the local address");
    }

    std::optional<Reply> outcome;
    std::array<uint8_t, kMaxDatagram> buf{};
    for (const auto wait : waits) {
        if (outcome || cancelled()) break;
        if (sendto(sock, reinterpret_cast<const char*>(request.data()),
                   static_cast<int>(request.size()), 0,
                   reinterpret_cast<const sockaddr*>(&gateway), sizeof(gateway)) < 0) {
            outcome = fail(protocol, "send to gateway failed");
            break;
        }
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (!outcome && !cancelled()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            const auto slice = std::min(
                kCancelPoll, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            set_receive_timeout(sock, slice);
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const auto n = recvfrom(sock, reinterpret_cast<char*>(buf.data()),
                                    static_cast<int>(buf.size()), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 0) {
                if (receive_timed_out()) continue;
                // An unreachable gateway surfaces here on some platforms;
                // the next retransmission tries again.
                while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::min(
                        kCancelPoll, std::chrono::duration_cast<std::chrono::milliseconds>(
                                         deadline - std::chrono::steady_clock::now())));
                }
                break;
            }
            if (from.sin_addr.s_addr != gateway.sin_addr.s_addr || from.sin_port != gateway.sin_port) {
                continue;
            }
            const std::span<const uint8_t> bytes(buf.data(), static_cast<size_t>(n));
            if (!accept(bytes)) continue;
            Reply ok;
            ok.ok = true;
            ok.value.assign(bytes.begin(), bytes.end());
            outcome = std::move(ok);
        }
    }
    close_socket(sock);
    if (!outcome) return fail(protocol, cancelled() ? "exchange cancelled" : "gateway did not answer");
    return std::move(*outcome);
}

}  // namespace lnsdk::udp_exchange
