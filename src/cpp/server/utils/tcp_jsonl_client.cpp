#include "lemon/utils/tcp_jsonl_client.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define INVALID_SOCKET_JL INVALID_SOCKET
    #define LMCLOSE_SOCKET ::closesocket
    #define SOCKET_ERRNO WSAGetLastError()
    #define SOCKET_EINTR WSAEINTR
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define INVALID_SOCKET_JL -1
    #define LMCLOSE_SOCKET ::close
    #define SOCKET_ERRNO errno
    #define SOCKET_EINTR EINTR
#endif

namespace lemon {
namespace utils {

namespace {

void ensure_socket_init() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    (void)initialized;
#endif
}

} // namespace

TcpJsonlClient::TcpJsonlClient() : socket_fd_(INVALID_SOCKET_JL) {}

TcpJsonlClient::~TcpJsonlClient() {
    close();
}

bool TcpJsonlClient::connect(const std::string& address, MessageCallback callback) {
    close();
    ensure_socket_init();

    // Parse "tcp://host:port"
    if (address.rfind("tcp://", 0) != 0) {
        return false;
    }
    std::string rest = address.substr(6); // after "tcp://"
    size_t colon = rest.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::string host = rest.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(rest.substr(colon + 1));
    } catch (...) {
        return false;
    }

    callback_ = std::move(callback);
    stop_.store(false);

    if (!do_connect(host, port)) {
        return false;
    }

    connected_.store(true);
    read_thread_ = std::thread(&TcpJsonlClient::read_loop, this, host, port);
    return true;
}

bool TcpJsonlClient::do_connect(const std::string& host, int port) {
    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_JL) {
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    // Backends hand out numeric loopback addresses; no hostname resolution.
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        LMCLOSE_SOCKET(sock);
        return false;
    }

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        LMCLOSE_SOCKET(sock);
        return false;
    }

    socket_fd_ = sock;
    return true;
}

void TcpJsonlClient::send(const json& msg) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ == INVALID_SOCKET_JL || !connected_.load()) {
        return;
    }

    std::string line = msg.dump() + "\n";
    const char* data = line.c_str();
    size_t remaining = line.size();

    while (remaining > 0) {
#ifdef _WIN32
        int sent = ::send(socket_fd_, data, static_cast<int>(remaining), 0);
#else
        auto sent = ::send(socket_fd_, data, remaining, 0);
#endif
        if (sent < 0) {
            if (SOCKET_ERRNO == SOCKET_EINTR) {
                continue;
            }
            connected_.store(false);
            return;
        }
        data += sent;
        remaining -= static_cast<size_t>(sent);
    }
}

void TcpJsonlClient::close() {
    stop_.store(true);
    shutdown_socket();

    if (read_thread_.joinable()) {
        read_thread_.join();
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ != INVALID_SOCKET_JL) {
        LMCLOSE_SOCKET(socket_fd_);
        socket_fd_ = INVALID_SOCKET_JL;
    }
    connected_.store(false);
}

void TcpJsonlClient::shutdown_socket() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ != INVALID_SOCKET_JL) {
#ifdef _WIN32
        shutdown(socket_fd_, SD_BOTH);
#else
        shutdown(socket_fd_, SHUT_RDWR);
#endif
    }
}

void TcpJsonlClient::read_loop(const std::string& host, int port) {
    (void)host;
    (void)port;

    std::string buffer;
    buffer.reserve(4096);

    while (!stop_.load() && connected_.load()) {
        char chunk[1024];
#ifdef _WIN32
        int n = recv(socket_fd_, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
        auto n = recv(socket_fd_, chunk, sizeof(chunk), 0);
#endif
        if (n <= 0) {
            if (n < 0 && SOCKET_ERRNO == SOCKET_EINTR) {
                continue;
            }
            break;
        }

        buffer.append(chunk, static_cast<size_t>(n));

        // Process complete lines
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            try {
                json msg = json::parse(line);
                if (callback_) {
                    callback_(msg);
                }
            } catch (const json::parse_error&) {
                // Malformed line, ignore
            }
        }

        // Prevent unbounded growth if no newlines ever arrive
        if (buffer.size() > 65536) {
            buffer.clear();
        }
    }

    connected_.store(false);
}

} // namespace utils
} // namespace lemon
