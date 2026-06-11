#pragma once

#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
#else
    typedef int SOCKET;
#endif

namespace lemon {
namespace utils {

using json = nlohmann::json;

/**
 * Minimal blocking TCP client for line-delimited JSON protocols.
 *
 * Connects to a TCP endpoint, sends JSON objects as newline-terminated
 * strings, and receives JSON objects the same way. Runs a background
 * thread for reading so the caller can send asynchronously.
 *
 * Thread-safe for send(). Must not outlive the message callback.
 */
class TcpJsonlClient {
public:
    using MessageCallback = std::function<void(const json&)>;

    TcpJsonlClient();
    ~TcpJsonlClient();

    // Non-copyable and non-movable: the read thread is bound to `this`, so a
    // moved-from object would leave the running thread referencing it.
    TcpJsonlClient(const TcpJsonlClient&) = delete;
    TcpJsonlClient& operator=(const TcpJsonlClient&) = delete;
    TcpJsonlClient(TcpJsonlClient&&) = delete;
    TcpJsonlClient& operator=(TcpJsonlClient&&) = delete;

    /**
     * Connect to a TCP endpoint.
     * @param address Format: "tcp://host:port" (e.g. "tcp://127.0.0.1:9002")
     * @param callback Called on the internal read thread for each received JSON line
     * @return true on success
     */
    bool connect(const std::string& address, MessageCallback callback);

    /**
     * Check if the client is currently connected.
     */
    bool is_connected() const { return connected_.load(); }

    /**
     * Send a JSON object. Thread-safe.
     */
    void send(const json& msg);

    /**
     * Close the connection and stop the read thread.
     */
    void close();

private:
    std::thread read_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    SOCKET socket_fd_;
    MessageCallback callback_;
    mutable std::mutex socket_mutex_;

    void read_loop(const std::string& host, int port);
    bool do_connect(const std::string& host, int port);
    void shutdown_socket();
};

} // namespace utils
} // namespace lemon
