#include "lemon/utils/tcp_jsonl_client.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define LMCLOSE_SOCKET ::closesocket
    #define SOCKET_ERRNO WSAGetLastError()
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define LMCLOSE_SOCKET ::close
    #define SOCKET_ERRNO errno
#endif

namespace lemon {
namespace utils {

TcpJsonlClient::TcpJsonlClient() = default;

TcpJsonlClient::~TcpJsonlClient() {
    close();
}

TcpJsonlClient::TcpJsonlClient(TcpJsonlClient&& other) noexcept
    : read_thread_(std::move(other.read_thread_)),
      connected_(other.connected_.exchange(false)),
      stop_(other.stop_.exchange(true)),
      socket_fd_(other.socket_fd_),
      callback_(std::move(other.callback_)) {
    other.socket_fd_ = -1;
}

TcpJsonlClient& TcpJsonlClient::operator=(TcpJsonlClient&& other) noexcept {
    if (this != &other) {
        close();
        read_thread_ = std::move(other.read_thread_);
        connected_ = other.connected_.exchange(false);
        stop_ = other.stop_.exchange(true);
        socket_fd_ = other.socket_fd_;
        callback_ = std::move(other.callback_);
        other.socket_fd_ = -1;
    }
    return *this;
}

bool TcpJsonlClient::connect(const std::string& address, MessageCallback callback) {
    close();

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
    socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        // Try resolving as hostname
        struct hostent* he = gethostbyname(host.c_str());
        if (!he || !he->h_addr_list[0]) {
            LMCLOSE_SOCKET(socket_fd_);
            socket_fd_ = -1;
            return false;
        }
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    }

    if (::connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        LMCLOSE_SOCKET(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    return true;
}

void TcpJsonlClient::send(const json& msg) {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ < 0 || !connected_.load()) {
        return;
    }

    std::string line = msg.dump() + "\n";
    const char* data = line.c_str();
    size_t remaining = line.size();

    while (remaining > 0) {
        ssize_t sent = ::send(socket_fd_, data, remaining, 0);
        if (sent < 0) {
            if (SOCKET_ERRNO == EINTR) {
                continue;
            }
            // Connection broken
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
    if (socket_fd_ >= 0) {
        LMCLOSE_SOCKET(socket_fd_);
        socket_fd_ = -1;
    }
    connected_.store(false);
}

void TcpJsonlClient::shutdown_socket() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ >= 0) {
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
        ssize_t n = recv(socket_fd_, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            if (n < 0 && SOCKET_ERRNO == EINTR) {
                continue;
            }
            // Connection closed or error
            break;
        }

        buffer.append(chunk, static_cast<size_t>(n));

        // Process complete lines
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            // Remove potential \r
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
