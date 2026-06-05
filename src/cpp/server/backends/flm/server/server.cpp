/*!
 *  Copyright (c) 2023 by Contributors
 * \file server.cpp
 * \brief WebServer class and related declarations
 * \author FastFlowLM Team
 * \date 2025-06-24
 *  \version 0.9.24
 */
#include "server.hpp"
#include "rest_handler.hpp"
#include <sstream>
#include <thread>
#include <iostream>
#include <iomanip>
#include <locale>


// Global NPU access control
std::mutex g_npu_access_mutex;
std::atomic<bool> g_npu_in_use{false};

std::atomic<int> g_npu_active_requests{0};

///@brief get current time string, format: hh:mm:ss mm:dd:yyyy
///@return the current time string
std::string get_current_time_string() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S %m:%d:%Y");
    return ss.str();
}

// Helper: Truncate a UTF-8 string by code points, not bytes
std::string utf8_truncate_middle(const std::string& input, size_t head_count, size_t tail_count) {
    // Walk the string to count code points
    size_t codepoints = 0;
    std::vector<size_t> positions; // positions of codepoint starts
    for (size_t i = 0; i < input.size();) {
        positions.push_back(i);
        unsigned char c = static_cast<unsigned char>(input[i]);
        size_t char_len = 1;
        if ((c & 0x80) == 0) char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;
        else char_len = 1; // fallback: treat as single byte
        i += char_len;
        codepoints++;
    }
    if (codepoints <= head_count + tail_count) return input;
    // Get head and tail
    std::string head = input.substr(0, positions[head_count]);
    std::string tail = input.substr(positions[codepoints - tail_count]);
    return head + "..." + tail;
}

///@brief brief print request
///@param request the request
void brief_print_message_request(nlohmann::json request) {
    // if request has "messages" or "message" field, the context in meassages shall be printed briefly

    // split ollama and openai message type of image

    if (request.contains("messages")) {
        for (auto& message : request["messages"]) {
            // Handle string-based content
            if (message["content"].is_string()) {
                std::string content = message["content"].get<std::string>();
                if (content.size() > 20) {
                    message["content"] = utf8_truncate_middle(content, 10, 10);
                }
            }
            // Handle array-based structured content
            else if (message["content"].is_array()) {
                for (auto& contentItem : message["content"]) {
                    if (contentItem.contains("type") && contentItem["type"] == "text") {
                        std::string text = contentItem["text"].get<std::string>();
                        if (text.size() > 20) {
                            contentItem["text"] = utf8_truncate_middle(text, 10, 10);
                        }
                    }
                    else if (contentItem.contains("type") && contentItem["type"] == "image_url") {
                        std::string image_url = contentItem["image_url"]["url"].get<std::string>();
                        if (image_url.size() > 20) {
                            contentItem["image_url"]["url"] = utf8_truncate_middle(image_url, 10, 10);
                        }
                    }
                    else if (contentItem.contains("type") && contentItem["type"] == "input_audio") {
                        std::string audio_base64 = contentItem["input_audio"]["data"].get<std::string>();
                        if (audio_base64.size() > 20) {
                            contentItem["input_audio"]["data"] = utf8_truncate_middle(audio_base64, 10, 10);
                        }
                    }
                }
            }
            // Handle legacy "images" array
            if (message.contains("images")) {
                nlohmann::ordered_json::array_t images = message.value("images", nlohmann::ordered_json::array());
                nlohmann::ordered_json::array_t new_images;
                for (auto& image : images) {
                    std::string image_str = image.get<std::string>();
                    if (image_str.size() > 20) {
                        new_images.push_back(utf8_truncate_middle(image_str, 10, 10));
                    }
                    else {
                        new_images.push_back(image_str);
                    }
                }
                message["images"] = new_images;
            }
        }
    }

    // TODO: improve tools logging like only print the tool name and elide the arguments
    // TODO: Support debug level
    if (request.contains("tools")) {
        request["tools"] = "...";
    }

    header_print("LOG", "Body: ");
    std::string brief_body = request.dump(4);
    std::cout << brief_body << std::endl;
}

///@brief brief print request
///@param request the request
void brief_print_message_response(nlohmann::json request) {

}

// NPU Access Manager implementation
bool NPUAccessManager::try_acquire_npu_access() {
    std::lock_guard<std::mutex> lock(g_npu_access_mutex);
    if (g_npu_in_use.load()) {
        return false; // NPU is already in use
    }
    g_npu_in_use.store(true);
    g_npu_active_requests.fetch_add(1);
    return true;
}

void NPUAccessManager::release_npu_access() {

    header_print("🔵 ", "NPU Lock Released!" );
    std::lock_guard<std::mutex> lock(g_npu_access_mutex);
    g_npu_in_use.store(false);
    g_npu_active_requests.fetch_sub(1);
}

bool NPUAccessManager::is_npu_available() {
    return !g_npu_in_use.load();
}

int NPUAccessManager::get_active_npu_requests() {
    return g_npu_active_requests.load();
}

// Helper function to check if an endpoint requires NPU access
bool requires_npu_access(const std::string& method, const std::string& path) {
    // NPU-intensive endpoints that should be restricted to one user at a time
    if (method == "POST") {
        return path == "/api/generate" ||
               path == "/api/chat" ||
               path == "/v1/chat/completions" ||
               path == "/v1/audio/transcriptions" ||
               path == "/v1/embeddings";
    }
    return false;
}

///@brief HttpSession class implementation
///@param socket the socket
///@param server the server
HttpSession::HttpSession(tcp::socket socket, WebServer& server)
    : socket_(std::move(socket))
    , server_(server)
    , is_streaming_(false) {
    // Set socket timeout
    socket_.set_option(tcp::socket::keep_alive(false));
    // Avoid abortive close that can lead to client-side broken pipe on large uploads
    socket_.set_option(tcp::socket::linger(false, 0));

    // Debug: Log TCP connection formation
    header_print("🔗 ", "TCP connection established - Remote: " + socket_.remote_endpoint().address().to_string() + ":" + std::to_string(socket_.remote_endpoint().port()));
}

///@brief start
void HttpSession::start(bool cors) {
    read_request(cors);
}

///@brief close connection
void HttpSession::close_connection() {
    boost::system::error_code ec;

    // Debug: Log TCP connection disconnection
    try {
        header_print("🔒 ", "TCP connection closing - Remote: " + socket_.remote_endpoint().address().to_string() + ":" + std::to_string(socket_.remote_endpoint().port()));
    } catch (const std::exception& e) {
        header_print("🔒 ", "TCP connection closing - Remote endpoint unavailable");
    }
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    server_.active_connections_.fetch_sub(1);
}

///@brief read request
void HttpSession::read_request(bool cors) {
    auto self = shared_from_this();

    // Use a parser to control the maximum accepted body size
    auto parser = std::make_shared<http::request_parser<http::string_body>>();
    parser->body_limit(self->server_.get_max_body_size_bytes());

    http::async_read(self->socket_, self->buffer_, *parser,
        [self, parser, cors](beast::error_code ec, std::size_t bytes_transferred) {
            if (!ec) {
                header_print("TCP", "Read " + std::to_string(bytes_transferred) + " bytes from socket");
                // Move the parsed message into our request object
                self->req_ = parser->release();
                self->handle_request(cors);
                return;
            }

            // Handle body too large explicitly with 413
            if (ec == http::error::body_limit) {
                self->res_ = {};
                self->res_.version(11);
                self->res_.result(http::status::payload_too_large);
                self->res_.set(http::field::content_type, "application/json");
                self->res_.keep_alive(false);
                json err = {{"error", "Request payload too large"}, {"max_bytes", self->server_.get_max_body_size_bytes()}};
                self->res_.body() = err.dump();
                self->res_.prepare_payload();
                self->write_response();
                return;
            }

            // Connection closed or other error, decrement connection counter
            try {
                header_print("🔒 ", "TCP connection closed - Remote: " + self->socket_.remote_endpoint().address().to_string() + ":" + std::to_string(self->socket_.remote_endpoint().port()));
            } catch (...) {
                header_print("🔒 ", "TCP connection closed - Remote endpoint unavailable");
            }
            self->server_.active_connections_.fetch_sub(1);
        });
}

///@brief handle request

void HttpSession::handle_request(bool cors) {
    // --- BEGIN: Preflight (OPTIONS) request handling ---
    if (req_.method() == http::verb::options && cors) {
        header_print("✈️ ", "Handling Preflight (OPTIONS) request");

        // reponse empty body for OPTIONS
        auto options_res = std::make_shared<http::response<http::empty_body>>();
        options_res->version(req_.version());
        options_res->result(http::status::ok);
        options_res->keep_alive(req_.keep_alive());

        options_res->set("Access-Control-Allow-Origin", "*");
        options_res->set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        options_res->set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");

        options_res->prepare_payload();
        auto self = shared_from_this();
        http::async_write(socket_, *options_res,
            [self, cors, options_res](beast::error_code ec, std::size_t) {
                if (ec) {
                    return;
                }
                // waiting the true response
                self->read_request(cors);
            });
        return;
    }
    // --- END: Preflight (OPTIONS) request handling ---


    // IF not OPTIONS
    // Reset response
    res_ = {};
    res_.version(req_.version());
    res_.keep_alive(req_.keep_alive());

    // Handle the request through the server
    bool deferred = server_.handle_request(req_, res_, socket_, shared_from_this());

    // Clear the buffer after processing to prevent data accumulation
    buffer_.consume(buffer_.size());

    if (!is_streaming_ && !deferred) {
        write_response();
    } else {
        // For streaming responses, we need to handle connection cleanup differently
        // The connection will be closed when streaming ends
    }
}

///@brief write response
void HttpSession::write_response() {
    auto self = shared_from_this();

    // header_print("⬆️ ", "Outgoing Response: ");
    // header_print("LOG", "Time stamp: " << get_current_time_string()); // hh:mm:ss mm:dd:yyyy

    // Check if this is one of the endpoints where we should skip printing the response body
    std::string target = std::string(req_.target());
    bool skip_body_print = (target == "/api/ps" || target == "/api/tags" || target == "/api/version" || target == "/v1/models" || target == "/api/show");

    //if (!skip_body_print) {
    //    try{
    //        nlohmann::json response_json = json::parse(res_.body());
    //        brief_print_message_request(response_json);
    //        // std::cout << "response_json: " << response_json.dump(4) << std::endl;
    //    } catch (const std::exception& e) {
    //        header_print("LOG", "Body: ");
    //        std::cout << res_.body() << std::endl;
    //    }
    //}

    std::cout << "================================================" << std::endl;

    res_.set("Access-Control-Allow-Origin", "*");
    res_.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res_.set("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");


    http::async_write(socket_, res_,
        [self](beast::error_code ec, std::size_t) {
            self->socket_.shutdown(tcp::socket::shutdown_both, ec);
            // Decrement connection counter for non-keep-alive connections
            self->server_.active_connections_.fetch_sub(1);
            try {
                header_print("🔒 ", "TCP connection closed - Remote: " + self->socket_.remote_endpoint().address().to_string() + ":" + std::to_string(self->socket_.remote_endpoint().port()));
            }
            catch (...) {
                header_print("🔒 ", "TCP connection closed - Remote endpoint unavailable");
            }

        });
}

///@brief write response from callback (for queued requests)
void HttpSession::write_response_from_callback() {
    // This function is called by the send_response lambda
    // when a queued request is finally processed.
    write_response();
}

void HttpSession::set_cancellation_token(std::shared_ptr<CancellationToken> token) {
    cancellation_token_ = token;
    start_client_disconnect_monitor(token);
}

void HttpSession::start_client_disconnect_monitor(std::shared_ptr<CancellationToken> token) {
    if (!token || token->cancelled() || !socket_.is_open()) {
        return;
    }

    auto self = shared_from_this();
    socket_.async_wait(tcp::socket::wait_read,
        [self, token](beast::error_code ec) {
            if (!token || token->cancelled() || token->completed()) {
                return;
            }

            if (ec) {
                if (ec != net::error::operation_aborted && !token->completed()) {
                    token->cancel();
                    header_print("🔴 ", "Client socket wait failed; cancelling active request: " + ec.message());
                }
                return;
            }

            char peek_buffer = 0;
            boost::system::error_code peek_ec;
            std::size_t bytes_read = self->socket_.receive(
                net::buffer(&peek_buffer, 1),
                boost::asio::socket_base::message_peek,
                peek_ec);

            if ((peek_ec || bytes_read == 0) && !token->completed()) {
                token->cancel();
                header_print("🔴 ", "Client disconnected; cancelling active request");
                return;
            }
        });
}

///@brief write streaming response
///@param data the data
///@param is_final the is final
void HttpSession::write_streaming_response(const json& data, bool is_final) {
    if (!is_streaming_) {
        // Initialize streaming response headers
        is_streaming_ = true;
        res_.result(http::status::ok);
        res_.set(http::field::content_type, "application/x-ndjson");
        res_.set(http::field::cache_control, "no-cache");
        res_.set(http::field::connection, "keep-alive");
        res_.set(http::field::transfer_encoding, "chunked");

        // Send headers immediately using raw socket write
        std::string headers = "HTTP/1.1 200 OK\r\n";
        headers += "Content-Type: text/event-stream\r\n";
        headers += "Cache-Control: no-cache\r\n";
        headers += "Connection: keep-alive\r\n";
        headers += "Transfer-Encoding: chunked\r\n";
        headers += "Access-Control-Allow-Origin: *\r\n";
        headers += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        headers += "Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With\r\n";
        headers += "\r\n";

        // Send headers synchronously
        boost::system::error_code ec;
        net::write(socket_, net::buffer(headers), ec);
        if (ec) return;
    }

    // Send this chunk immediately
    send_chunk_data(data, is_final);
}

///@brief send chunk data
///@param data the data
///@param is_final the is final
void HttpSession::send_chunk_data(const json& data, bool is_final) {
    std::string chunk_content;

    // Check if the data is a string (pre-formatted SSE with "data: " prefix)
    if (data.is_string()) {
        std::string data_str = data.get<std::string>();
        // If it starts with "data: ", it's already formatted for SSE
        if (data_str.substr(0, 6) == "data: ") {
            chunk_content = data_str;
        } else {
            // Regular JSON string, add single newline for Ollama format
            chunk_content = data_str;
        }
    } else {
        // Regular JSON object, format for Ollama compatibility
        chunk_content = data.dump() + "\n";
    }

    // std::cout << "Chunk: " << chunk_content;

    // HTTP chunked format: size in hex + \r\n + data + \r\n
    std::ostringstream chunk_size;
    chunk_size << std::hex << chunk_content.length();
    std::string http_chunk;
    //if(is_final)
    //    http_chunk = chunk_content + "\r\n";
    //else
    http_chunk = chunk_size.str() + "\r\n" + chunk_content + "\r\n";


    // Send chunk immediately
    boost::system::error_code ec;
    net::write(socket_, net::buffer(http_chunk), ec);

    if (ec) {
        if (cancellation_token_ && !cancellation_token_->completed()) {
            cancellation_token_->cancel();
        }

        //socket_.shutdown(tcp::socket::shutdown_both, ec);
        //server_.active_connections_.fetch_sub(1);

        boost::system::error_code ignore_ec;
        socket_.close(ignore_ec);
        return;
    }

    if (is_final) {
        // Send final chunk (0-length chunk to end stream)
        std::string final_chunk = "0\r\n\r\n";
        net::write(socket_, net::buffer(final_chunk), ec);
        header_print("🔒 ", "Closing TCP connection (streaming, non-keep-alive)");
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        // Decrement connection counter for non-keep-alive connections
        server_.active_connections_.fetch_sub(1);
    }
}

///@brief WebServer implementation
///@param port the port
WebServer::WebServer(std::string host, int port, bool cors) : acceptor(ioc, {net::ip::make_address(host), static_cast<unsigned short>(port)}), running(false), port(port), cors(cors) {}

///@brief destructor
WebServer::~WebServer() {
    stop();
}

///@brief start
void WebServer::start() {
    running = true;
    do_accept();

    // Run the I/O service on multiple threads for better concurrency
    for (size_t i = 0; i < io_thread_count_; ++i) {
        io_threads_.emplace_back([this]() {
            try {
                ioc.run();
            } catch (const std::exception& e) {
                header_print("LOG", "Error in WebServer I/O thread: " + std::string(e.what()));
            }
        });
    }

    header_print("FLM", "WebServer started on port " + std::to_string(port) + " with " + std::to_string(io_thread_count_) + " I/O threads");
}

///@brief stop
void WebServer::stop() {
    running = false;
    ioc.stop();

    // Wait for all I/O threads to finish
    for (auto& thread : io_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    io_threads_.clear();
}

///@brief register active request
///@param request_id the request ID
///@param token the cancellation token
void WebServer::register_active_request(const std::string& request_id, std::shared_ptr<CancellationToken> token) {
    std::lock_guard<std::mutex> lock(active_requests_mutex_);
    active_requests_[request_id] = token;
}

///@brief unregister active request
///@param request_id the request ID
void WebServer::unregister_active_request(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(active_requests_mutex_);
    active_requests_.erase(request_id);
}

///@brief cancel request
///@param request_id the request ID
///@return true if request was found and cancelled
bool WebServer::cancel_request(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(active_requests_mutex_);
    auto it = active_requests_.find(request_id);
    if (it != active_requests_.end()) {
        it->second->cancel();
        active_requests_.erase(it);
        return true;
    }
    return false;
}

///@brief register handler
///@param method the method
///@param path the path
///@param handler the handler
void WebServer::register_handler(const std::string& method, const std::string& path, RequestHandler handler) {
    std::string key = method + " " + path;
    routes[key] = handler;
}

///@brief do accept
void WebServer::do_accept() {
    acceptor.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                // Check connection limit
                if (active_connections_.load() >= max_connections_) {
                    header_print("LOG", "Connection limit reached (" + std::to_string(max_connections_) + "), rejecting new connection");
                    // Close the socket and continue accepting
                    socket.close();
                } else {
                    // Increment connection counter
                    active_connections_.fetch_add(1);

                    // Create a new session for this connection
                    auto session = std::make_shared<HttpSession>(std::move(socket), *this);
                    session->start(cors);
                }
            }
            if (running) {
                do_accept();
            }
        });
}

///@brief process_next_npu_request Handles one queued NPU task at a time
void WebServer::process_next_npu_request() {
    {
        std::lock_guard<std::mutex> lock(npu_queue_mutex_);
    if (npu_request_queue_.empty()) {
        NPUAccessManager::release_npu_access();
        return; // Queue is empty, NPU is free
    }
    }

    // NPU cooldown before running the next queued task.
    constexpr auto npu_cooldown = std::chrono::milliseconds(333);
    std::this_thread::sleep_for(npu_cooldown);

    std::function<void()> task;
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(npu_queue_mutex_);
        if (npu_request_queue_.empty()) {
            NPUAccessManager::release_npu_access();
            return;
        }

        task = npu_request_queue_.front();
        npu_request_queue_.pop();
        remaining = npu_request_queue_.size();
    }

    header_print("🟡 ", "Dequeuing NPU request (" + std::to_string(remaining) + " remaining)...");

    // Post the task to be executed by the io_context
    net::post(ioc, task);

}

///@brief handle request
///@param req the request
///@param res the response
///@param socket the socket
///@param session the session
///@return true if the response is deferred (queued), false otherwise
bool WebServer::handle_request(http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    tcp::socket& socket,
    std::shared_ptr<HttpSession> session) {
    // Log request details
    std::cout << "================================================" << std::endl;
    header_print("⬇️ ", "Incoming Request: " << req.method_string());
    header_print("LOG", "Time stamp: " << get_current_time_string()); // hh:mm:ss mm:dd:yyyy
    header_print("LOG", "Target: " << req.target());
    header_print("LOG", "Version: " << req.version());
    header_print("LOG", "Keep-Alive: " << req.keep_alive());
    json request_json_log;
    bool is_json = false;
    try {
        if (!req.body().empty()) {
            std::string content_type = std::string(req[http::field::content_type]);

            if (content_type.find("application/json") != std::string::npos) {
                json request_json_log = json::parse(req.body());
                brief_print_message_request(request_json_log);
                is_json = true;
            }
            else if (content_type.find("multipart/form-data") != std::string::npos) {
                // print some request info
            }
        }
    }
    catch (const std::exception& e) {
        header_print("LOG", "Error parsing request body: " + std::string(e.what()));
    }

    // Route lookup
    std::string key = std::string(req.method_string()) + " " + std::string(req.target());
    auto it = routes.find(key);
    if (it == routes.end()) {
        // No route: respond 404 synchronously.
        res.result(http::status::not_found);
        res.body() = json{ {"error", "Not Found"} }.dump();
        res.set(http::field::content_type, is_json ? "application/json" : "multipart/form-data");
        res.prepare_payload();
        return false;
    }

    // Decide if this handler needs exclusive NPU access.
    bool needs_npu = requires_npu_access(std::string(req.method_string()), std::string(req.target()));

    // Store stable pointers for deferred execution to avoid reference lifetime issues.
    auto* req_ptr = &req;
    auto* res_ptr = &res;

    // Define a task lambda with is_deferred flag
    auto process_task = [this, it, req_ptr, res_ptr, session, needs_npu, key, is_json](bool is_deferred) {
        auto& req_ref = *req_ptr;
        auto& res_ref = *res_ptr;

        // Parse JSON request body
        json request_json;
        try {
            if (!req_ref.body().empty()) {
                if(is_json)
                    request_json = json::parse(req_ref.body());
                //else
                    //do some parse to the MultiPart request?
                    //std::map<std::string, MultipartPart> parts = parse_multipart(req_ref);
            }
        }
        catch (const std::exception& e) {
            res_ref.result(http::status::bad_request);
            res_ref.body() = json{ {"error", "Invalid JSON"} }.dump();
            res_ref.set(http::field::content_type, "application/json");
            res_ref.prepare_payload();

            // Only write from callback when deferred
            if (is_deferred && session) session->write_response_from_callback();

            if (needs_npu) {
                this->process_next_npu_request();
            }
            return;
        }

        auto cancellation_token = std::make_shared<CancellationToken>(session);
        session->set_cancellation_token(cancellation_token);

        std::string request_id;
        if (request_json.contains("request_id")) {
            request_id = request_json["request_id"];
        }
        else {
            static std::atomic<int> counter{ 0 };
            request_id = "req_" + std::to_string(counter.fetch_add(1));
        }

        register_active_request(request_id, cancellation_token);

        // catch is_deferred
        auto send_response = [res_ptr, session, this, request_id, needs_npu, is_deferred, cancellation_token](const json& response_data) {
            auto& response_ref = *res_ptr;
            http::status status = http::status::ok;

            if (response_data.contains("error") &&
                response_data["error"].contains("code"))
            {
                int code = response_data["error"]["code"].get<int>();

                if (code == 400) {
                    status = http::status::bad_request;
                }
                //else if () {

                //}
            }

            response_ref.result(status);
            response_ref.body() = response_data.dump();
            response_ref.set(http::field::content_type, "application/json");
            response_ref.prepare_payload();
            cancellation_token->complete();
            unregister_active_request(request_id);

            if (needs_npu) {
                this->process_next_npu_request();
            }

            if (is_deferred && session) {
                session->write_response_from_callback();
            }
        };

        auto send_streaming_response = [session, this, request_id, needs_npu, cancellation_token](const json& data, bool is_final) {
            if (is_final) {
                cancellation_token->complete();
            }

            if (session) {
                session->write_streaming_response(data, is_final);
            }
            if (is_final) {
                unregister_active_request(request_id);

                if (needs_npu) {
                    this->process_next_npu_request();
                }
            }
        };

        try {
            it->second(req_ref, send_response, send_streaming_response, session, cancellation_token);
        }
        catch (const std::exception& e) {
            cancellation_token->complete();
            unregister_active_request(request_id);

            res_ref.result(http::status::internal_server_error);
            res_ref.body() = json{ {"error", std::string("Handler exception: ") + e.what()} }.dump();
            res_ref.set(http::field::content_type, "application/json");
            res_ref.prepare_payload();

            if (needs_npu) {
                this->process_next_npu_request();
            }

            if (is_deferred && session) {
                session->write_response_from_callback();
            }
        }
        catch (...) {
            cancellation_token->complete();
            unregister_active_request(request_id);

            res_ref.result(http::status::internal_server_error);
            res_ref.body() = json{ {"error", "Unknown handler exception"} }.dump();
            res_ref.set(http::field::content_type, "application/json");
            res_ref.prepare_payload();

            if (needs_npu) {
                this->process_next_npu_request();
            }

            if (is_deferred && session) {
                session->write_response_from_callback();
            }
        }
    }; // --- End of process_task lambda ---


    // --- Logic to run or queue the task ---

    if (!needs_npu) {
        process_task(false); //  return false
        return false;
    }

    if (NPUAccessManager::try_acquire_npu_access()) {
        if (needs_npu) {
            header_print("🟢 ", "NPU Locked!");
        }
        process_task(false); //  return false
        return false;
    }

    //const int NPU_QUEUE_LIMIT = 10;
    std::lock_guard<std::mutex> lock(npu_queue_mutex_);

    if (npu_request_queue_.size() >= max_npu_queue_) {
        res.result(http::status::service_unavailable);
        res.body() = json{
            {"error", "NPU is in use and request queue is full (limit: " + std::to_string(max_npu_queue_) + "). Please try again later."}
        }.dump();
        res.set(http::field::content_type, "application/json");
        res.prepare_payload();
        header_print("🚫 ", "NPU busy and queue full, request denied: " + key);
        return false;
    }
    else {
        // Create a new lambda to bind process_task(true)
        npu_request_queue_.push([this, process_task]() {
            process_task(true);
            });
        header_print("🕒 ", "NPU busy, request queued (" + std::to_string(npu_request_queue_.size()) + "/" + std::to_string(max_npu_queue_) + "): " + key);

        return true;
    }
}

///@brief create lm server
///@param models the model list
///@param downloader the downloader
///@param default_tag the default tag
///@param port the port
///@return the server
std::unique_ptr<WebServer> create_lm_server(model_list& models, ModelDownloader& downloader, program_args_t& args) {
    auto server = std::make_unique<WebServer>(args.host, args.port, args.cors);
    auto rest_handler = std::make_shared<RestHandler>(models, downloader, args);

    // Register Ollama-compatible routes
    server->register_handler("POST", "/api/show",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                json request_json;
                if (!req.body().empty()) {
                    request_json = json::parse(req.body());
                }
                rest_handler->handle_show(request_json, send_response, send_streaming_response);
        });

    server->register_handler("POST", "/api/generate",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }
            rest_handler->handle_generate(request_json, send_response, send_streaming_response, cancellation_token);
        });

    server->register_handler("POST", "/api/chat",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }
            rest_handler->handle_chat(request_json, send_response, send_streaming_response, cancellation_token);
        });

    server->register_handler("GET", "/api/ps",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            rest_handler->handle_ps(request_json, send_response, send_streaming_response);
        });

    server->register_handler("POST", "/api/embeddings",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }
            rest_handler->handle_embeddings(request_json, send_response, send_streaming_response);
        });

    server->register_handler("GET", "/api/tags",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            rest_handler->handle_models(request_json, send_response, send_streaming_response);
        });

    server->register_handler("GET", "/api/version",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            rest_handler->handle_version(request_json, send_response, send_streaming_response);
        });

    // Add NPU status endpoint
    server->register_handler("GET", "/api/npu/status",
        [](const http::request<http::string_body>& req,
           std::function<void(const json&)> send_response,
           std::function<void(const json&, bool)> send_streaming_response,
           std::shared_ptr<HttpSession> session,
           std::shared_ptr<CancellationToken> cancellation_token) {
            json response = {
                {"npu_available", NPUAccessManager::is_npu_available()},
                {"active_requests", NPUAccessManager::get_active_npu_requests()},
                {"message", NPUAccessManager::is_npu_available() ? "NPU is available" : "NPU is currently in use"}
            };
            send_response(response);
        });

    // Add other endpoints...
    server->register_handler("POST", "/api/pull",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }
            rest_handler->handle_pull(request_json, send_response, send_streaming_response);
        });

    // Add OpenAI endpoints
    server->register_handler("GET", "/v1/version",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                json request_json;
                rest_handler->handle_version(request_json, send_response, send_streaming_response);
        });

    server->register_handler("GET", "/v1/models",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                json request_json;
                rest_handler->handle_models_openai(request_json, send_response, send_streaming_response);
        });

    server->register_handler("POST", "/v1/embeddings",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                json request_json;
                if (!req.body().empty()) {
                    request_json = json::parse(req.body());
                }
                rest_handler->handle_embeddings(request_json, send_response, send_streaming_response);
        });

    server->register_handler("POST", "/v1/chat/completions",
        [rest_handler](const http::request<http::string_body>& req,
                      std::function<void(const json&)> send_response,
                      std::function<void(const json&, bool)> send_streaming_response,
                      std::shared_ptr<HttpSession> session,
                      std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }
            rest_handler->handle_openai_chat_completion(request_json, send_response, send_streaming_response, cancellation_token);
        });

    server->register_handler("POST", "/v1/audio/transcriptions",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                std::map<std::string, MultipartPart> parts = parse_multipart(req);
                json request_json;
                request_json["model"] = parts["model"].content;
                request_json["file"] = parts["file"].content;
                rest_handler->handle_openai_audio_transcriptions(request_json, send_response, send_streaming_response, cancellation_token);
        });

    server->register_handler("POST", "/v1/completions",
        [rest_handler](const http::request<http::string_body>& req,
            std::function<void(const json&)> send_response,
            std::function<void(const json&, bool)> send_streaming_response,
            std::shared_ptr<HttpSession> session,
            std::shared_ptr<CancellationToken> cancellation_token) {
                json request_json;
                if (!req.body().empty()) {
                    request_json = json::parse(req.body());
                }
                rest_handler->handle_openai_completion(request_json, send_response, send_streaming_response, cancellation_token);
        });


    // Add cancel endpoint - capture server by raw pointer
    WebServer* server_ptr = server.get();
    server->register_handler("POST", "/api/cancel",
        [server_ptr](const http::request<http::string_body>& req,
                     std::function<void(const json&)> send_response,
                     std::function<void(const json&, bool)> send_streaming_response,
                     std::shared_ptr<HttpSession> session,
                     std::shared_ptr<CancellationToken> cancellation_token) {
            json request_json;
            if (!req.body().empty()) {
                request_json = json::parse(req.body());
            }

            if (!request_json.contains("request_id")) {
                json error_response = {{"error", "request_id is required"}};
                send_response(error_response);
                return;
            }

            std::string request_id = request_json["request_id"];

            // Try to cancel the request
            bool cancelled = server_ptr->cancel_request(request_id);

            json response;
            if (cancelled) {
                response["cancelled"] = true;
                response["message"] = "Request cancelled successfully";
            } else {
                response["cancelled"] = false;
                response["message"] = "Request not found or already completed";
            }
            send_response(response);
        });

    return server;
}
