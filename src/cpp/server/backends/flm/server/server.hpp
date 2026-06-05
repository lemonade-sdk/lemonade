/*!
 *  Copyright (c) 2023 by Contributors
 * \file server.hpp
 * \brief WebServer class and related declarations
 * \author FastFlowLM Team
 * \date 2025-06-24
 *  \version 0.9.24
 */
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "wstream_buf.hpp"
#include "program_args.hpp"
#include "streaming_ostream.hpp"
#include "model_downloader.hpp"
#include "multipart.hpp"
#include <queue>
#include <functional>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::ordered_json;

// Forward declaration
class RestHandler;
class HttpSession;

// Global NPU access control
extern std::mutex g_npu_access_mutex;
extern std::atomic<bool> g_npu_in_use;
extern std::atomic<int> g_npu_active_requests;

// Helper function to check if an endpoint requires NPU access
bool requires_npu_access(const std::string& method, const std::string& path);

///@brief get current time string, format: hh:mm:ss mm:dd:yyyy
///@return the current time string
std::string get_current_time_string();

// NPU access manager class
class NPUAccessManager {
public:
    static bool try_acquire_npu_access();
    static void release_npu_access();
    static bool is_npu_available();
    static int get_active_npu_requests();
};

// Stream response callback type for handling streaming responses
using StreamCallback = std::function<void(const std::string&)>;

// Cancellation token for request cancellation
struct CancellationToken {
    std::atomic<bool> is_cancelled;
    std::atomic<bool> is_completed;
    std::weak_ptr<HttpSession> session;

    CancellationToken(std::shared_ptr<HttpSession> s) : is_cancelled(false), is_completed(false), session(s) {}

    void cancel() {
        is_cancelled.store(true);
    }
    void complete() {
        is_completed.store(true);
    }
    void reset() noexcept {
        is_cancelled.store(false, std::memory_order_relaxed);
        is_completed.store(false, std::memory_order_relaxed);
    }
    bool cancelled() const {
        return is_cancelled.load();
    }
    bool completed() const {
        return is_completed.load();
    }
};

// Request handler callback type
using RequestHandler = std::function<void(
    const http::request<http::string_body>& req,
    std::function<void(const json&)> send_response,
    std::function<void(const json&, bool)> send_streaming_response,  // data, is_final
    std::shared_ptr<HttpSession> session,  // for streaming support
    std::shared_ptr<CancellationToken> cancellation_token  // for cancellation support
)>;


void brief_print_message_request(nlohmann::json request);

class WebServer {
public:
    WebServer(std::string host, int port, bool cors);
    ~WebServer();

    void start();
    void stop();

    // Configuration methods for concurrency
    void set_max_connections(size_t max_conns) { max_connections_ = max_conns; }
    void set_request_timeout(std::chrono::seconds timeout) { request_timeout_ = timeout; }
    void set_io_threads(size_t num_threads) { io_thread_count_ = num_threads; }
    void set_npu_queue_length(size_t q_len) { max_npu_queue_ = q_len; }
    // Maximum accepted HTTP request body size (in bytes)
    void set_max_body_size_bytes(std::size_t bytes) { max_body_size_bytes_ = bytes; }
    std::size_t get_max_body_size_bytes() const { return max_body_size_bytes_; }

    void register_handler(const std::string& method, const std::string& path, RequestHandler handler);

    bool handle_request(http::request<http::string_body>& req,
                       http::response<http::string_body>& res,
                       tcp::socket& socket,
                       std::shared_ptr<HttpSession> session);

    // Request tracking methods
    void register_active_request(const std::string& request_id, std::shared_ptr<CancellationToken> token);
    void unregister_active_request(const std::string& request_id);
    bool cancel_request(const std::string& request_id);

    // Statistics
    size_t get_active_connections() const { return active_connections_.load(); }
    size_t get_active_requests() const {
        std::lock_guard<std::mutex> lock(active_requests_mutex_);
        return active_requests_.size();
    }

private:
    ///@brief do accept
    void do_accept();
    void process_next_npu_request();
    ///@brief io context
    net::io_context ioc;
    ///@brief acceptor
    tcp::acceptor acceptor;
    ///@brief routes
    std::map<std::string, RequestHandler> routes;
    ///@brief running
    bool running;
    ///@brief port
    int port;
    bool cors;

    // Concurrency configuration
    size_t max_connections_ = 10;
    std::chrono::seconds request_timeout_ = std::chrono::seconds(600); // 5 minutes
    size_t io_thread_count_ = 5;
    std::size_t max_body_size_bytes_ = 256ull * 1024 * 1024; // 256 MB default
    size_t max_npu_queue_ = 10;

    // Request tracking
    mutable std::mutex active_requests_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<CancellationToken>> active_requests_;

    // Connection tracking
    std::atomic<size_t> active_connections_{0};
    std::vector<std::thread> io_threads_;
    std::queue<std::function<void()>> npu_request_queue_;
    std::mutex npu_queue_mutex_;
    // Friend declaration for HttpSession to access private members
    friend class HttpSession;
};

///@brief HttpSession class for handling individual connections
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, WebServer& server);
    void start(bool cors);
    void write_streaming_response(const json& data, bool is_final);
    void close_connection();
    void write_response_from_callback();
    void set_cancellation_token(std::shared_ptr<CancellationToken> token);
private:
    void read_request(bool cors);
    void handle_request(bool cors);
    void write_response();
    void send_chunk_data(const json& data, bool is_final);
    void start_client_disconnect_monitor(std::shared_ptr<CancellationToken> token);

    ///@brief socket
    tcp::socket socket_;
    ///@brief buffer
    beast::flat_buffer buffer_;
    ///@brief request
    http::request<http::string_body> req_;
    ///@brief response
    http::response<http::string_body> res_;
    ///@brief server
    WebServer& server_;
    ///@brief is streaming
    bool is_streaming_;
    ///@brief stream buffer
    std::shared_ptr<streaming_buf> stream_buf_;
    std::shared_ptr<CancellationToken> cancellation_token_;
};

// Forward declarations
class model_list;

///@brief create lm server
///@param models the model list
///@param downloader the downloader
///@param default_tag the default tag
///@param port the port to listen on, default is 52625, same with the ollama server
///@return the server
std::unique_ptr<WebServer> create_lm_server(model_list& models, ModelDownloader& downloader, program_args_t& args);
