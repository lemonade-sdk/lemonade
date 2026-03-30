#include "lemon/websocket_server.h"

#include "lemon/router.h"
#include "lemon/utils/process_manager.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <utility>

#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

static struct lws_protocols protocols[] = {
    {"lemonade-realtime", WebSocketServer::ws_callback, sizeof(PerSessionData), 65536, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

std::string random_ticket() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    std::ostringstream stream;
    stream << std::hex << dist(gen) << dist(gen);
    return stream.str();
}

} // namespace

WebSocketServer::WebSocketServer(Router* router, const std::string& host, int requested_port)
    : port_(requested_port > 0 ? requested_port : utils::ProcessManager::find_free_port(9000)),
      host_(host),
      router_(router),
      session_manager_(std::make_unique<RealtimeSessionManager>(router)) {
    LOG(INFO, "WebSocket") << "Configured port: " << port_ << std::endl;
}

WebSocketServer::~WebSocketServer() {
    stop();
}

int WebSocketServer::ws_callback(struct lws* wsi,
                                 enum lws_callback_reasons reason,
                                 void* user,
                                 void* in,
                                 size_t len) {
    struct lws_context* ctx = lws_get_context(wsi);
    if (!ctx) {
        return 0;
    }

    auto* server = static_cast<WebSocketServer*>(lws_context_user(ctx));
    if (!server) {
        return 0;
    }

    auto* pss = static_cast<PerSessionData*>(user);

    switch (reason) {
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
            const std::string path = get_request_path(wsi);
            const auto kind = classify_path(path);
            if (kind == ConnectionKind::invalid) {
                return 1;
            }

            if (kind == ConnectionKind::logs) {
                if (!get_url_arg(wsi, "ticket")) {
                    return 1;
                }
            }
            break;
        }

        case LWS_CALLBACK_ESTABLISHED: {
            std::snprintf(pss->connection_id, sizeof(pss->connection_id), "%d", static_cast<int>(lws_get_socket_fd(wsi)));

            char ip[128] = {0};
            lws_get_peer_simple(wsi, ip, sizeof(ip));
            LOG(INFO, "WebSocket") << "New connection from: " << ip
                                   << " (id: " << pss->connection_id << ")" << std::endl;

            server->handle_connection(pss->connection_id, wsi);
            break;
        }

        case LWS_CALLBACK_CLOSED:
            server->handle_close(pss->connection_id);
            break;

        case LWS_CALLBACK_RECEIVE: {
            if (!in || len == 0) {
                break;
            }

            std::string conn_id(pss->connection_id);

            {
                std::lock_guard<std::mutex> lock(server->connections_mutex_);
                auto state_it = server->connection_states_.find(conn_id);
                if (state_it == server->connection_states_.end() ||
                    state_it->second.kind != ConnectionKind::realtime) {
                    break;
                }
                server->receive_buffers_[conn_id].append(static_cast<const char*>(in), len);
            }

            if (lws_remaining_packet_payload(wsi) == 0 && lws_is_final_fragment(wsi)) {
                std::string complete_msg;
                {
                    std::lock_guard<std::mutex> lock(server->connections_mutex_);
                    complete_msg = std::move(server->receive_buffers_[conn_id]);
                    server->receive_buffers_[conn_id].clear();
                }
                server->handle_message(conn_id, complete_msg);
            }
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE:
            server->handle_writable(pss->connection_id, wsi);
            break;

        default:
            break;
    }

    return 0;
}

bool WebSocketServer::start() {
    if (running_.load()) {
        return true;
    }

    struct lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));

    info.port = port_;
    info.protocols = protocols;
    info.user = this;

    if (!host_.empty() && host_ != "0.0.0.0") {
        if (host_ == "localhost") {
            info.iface = "127.0.0.1";
        } else {
            info.iface = host_.c_str();
        }
    }

    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

    context_ = lws_create_context(&info);
    if (!context_) {
        LOG(ERROR, "WebSocket") << "Failed to create context on port " << port_ << std::endl;
        return false;
    }

    running_.store(true);
    service_thread_ = std::thread(&WebSocketServer::service_loop, this);

    LOG(INFO, "WebSocket") << "Server started on port " << port_ << std::endl;
    return true;
}

void WebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    if (context_) {
        lws_cancel_service(context_);
    }

    if (service_thread_.joinable()) {
        service_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [_, state] : connection_states_) {
            if (!state.realtime_session_id.empty()) {
                session_manager_->close_session(state.realtime_session_id);
            }
            if (!state.log_subscriber_id.empty()) {
                LogStreamHub::instance().remove_subscriber(state.log_subscriber_id);
            }
        }

        connection_states_.clear();
        connection_websockets_.clear();
        message_queues_.clear();
        receive_buffers_.clear();
        log_tickets_.clear();
    }

    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }

    LOG(INFO, "WebSocket") << "Server stopped" << std::endl;
}

json WebSocketServer::mint_log_ticket(std::optional<uint64_t> after_seq) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    cleanup_expired_tickets_locked();

    std::string ticket = random_ticket();
    log_tickets_[ticket] = {
        after_seq,
        std::chrono::steady_clock::now() + std::chrono::seconds(30),
    };

    return {
        {"ticket", ticket},
        {"expires_at", static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch() + std::chrono::seconds(30)).count())},
        {"after_seq", after_seq.has_value() ? json(*after_seq) : json(nullptr)},
        {"path", "/logs/stream"},
        {"websocket_port", port_},
    };
}

void WebSocketServer::service_loop() {
    while (running_.load()) {
        lws_service(context_, 50);
        schedule_pending_writes();
    }
}

void WebSocketServer::handle_connection(const std::string& connection_id, struct lws* wsi) {
    const std::string path = get_request_path(wsi);
    const auto kind = classify_path(path);
    const auto params = extract_params(wsi, kind);

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_websockets_[connection_id] = wsi;
        connection_states_[connection_id] = {};
        connection_states_[connection_id].kind = kind;
    }

    switch (kind) {
        case ConnectionKind::realtime:
            handle_realtime_connection(connection_id, wsi, params);
            break;
        case ConnectionKind::logs: {
            const auto ticket_it = params.find("ticket");
            if (ticket_it == params.end()) {
                send_json(connection_id, {
                    {"type", "error"},
                    {"error", {{"message", "Missing log stream ticket"}, {"type", "invalid_request_error"}}},
                });
                return;
            }

            auto ticket = consume_log_ticket(ticket_it->second);
            if (!ticket.has_value()) {
                send_json(connection_id, {
                    {"type", "error"},
                    {"error", {{"message", "Invalid or expired log stream ticket"}, {"type", "invalid_request_error"}}},
                });
                return;
            }

            handle_log_connection(connection_id, wsi, *ticket);
            break;
        }
        case ConnectionKind::invalid:
            send_json(connection_id, {
                {"type", "error"},
                {"error", {{"message", "Unsupported websocket path"}, {"type", "invalid_request_error"}}},
            });
            break;
    }
}

void WebSocketServer::handle_realtime_connection(
    const std::string& connection_id,
    struct lws*,
    const std::unordered_map<std::string, std::string>& params) {
    json initial_config = json::object();
    if (params.count("model")) {
        initial_config["model"] = params.at("model");
    }

    auto send_callback = [this, connection_id](const json& msg) {
        send_json(connection_id, msg);
    };

    const std::string session_id = session_manager_->create_session(send_callback, initial_config);

    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto& state = connection_states_[connection_id];
    state.kind = ConnectionKind::realtime;
    state.realtime_session_id = session_id;
}

void WebSocketServer::handle_log_connection(const std::string& connection_id,
                                            struct lws*,
                                            std::optional<uint64_t> after_seq) {
    const auto snapshot_entries = LogStreamHub::instance().snapshot(after_seq);
    const std::string subscriber_id = LogStreamHub::instance().add_subscriber(
        [this, connection_id](const LogStreamEntry& entry) {
            send_json(connection_id, {
                {"type", "logs.entry"},
                {"entry", entry.to_json()},
            });
        });

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto& state = connection_states_[connection_id];
        state.kind = ConnectionKind::logs;
        state.log_subscriber_id = subscriber_id;
    }

    json entries_json = json::array();
    for (const auto& entry : snapshot_entries) {
        entries_json.push_back(entry.to_json());
    }

    send_json(connection_id, {
        {"type", "logs.snapshot"},
        {"entries", entries_json},
    });
}

void WebSocketServer::handle_message(const std::string& connection_id, const std::string& msg) {
    std::string session_id;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_states_.find(connection_id);
        if (it == connection_states_.end()) {
            return;
        }

        if (it->second.kind != ConnectionKind::realtime) {
            send_json(connection_id, {
                {"type", "error"},
                {"error", {{"message", "Log stream sockets are receive-only"}, {"type", "invalid_request_error"}}},
            });
            return;
        }

        session_id = it->second.realtime_session_id;
    }

    json request;
    try {
        request = json::parse(msg);
    } catch (const json::parse_error& e) {
        send_json(connection_id, {
            {"type", "error"},
            {"error", {{"message", "Invalid JSON: " + std::string(e.what())}, {"type", "invalid_request_error"}}},
        });
        return;
    }

    const std::string msg_type = request.value("type", "");

    if (msg_type == "session.update") {
        session_manager_->update_session(session_id, request.value("session", json::object()));
    } else if (msg_type == "input_audio_buffer.append") {
        const std::string audio = request.value("audio", "");
        if (!audio.empty()) {
            session_manager_->append_audio(session_id, audio);
        }
    } else if (msg_type == "input_audio_buffer.commit") {
        session_manager_->commit_audio(session_id);
    } else if (msg_type == "input_audio_buffer.clear") {
        session_manager_->clear_audio(session_id);
    } else {
        send_json(connection_id, {
            {"type", "error"},
            {"error", {{"message", "Unknown message type: " + msg_type}, {"type", "invalid_request_error"}}},
        });
    }
}

void WebSocketServer::handle_close(const std::string& connection_id) {
    ConnectionState state;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto state_it = connection_states_.find(connection_id);
        if (state_it != connection_states_.end()) {
            state = state_it->second;
            connection_states_.erase(state_it);
        }

        connection_websockets_.erase(connection_id);
        message_queues_.erase(connection_id);
        receive_buffers_.erase(connection_id);
    }

    if (!state.realtime_session_id.empty()) {
        session_manager_->close_session(state.realtime_session_id);
    }
    if (!state.log_subscriber_id.empty()) {
        LogStreamHub::instance().remove_subscriber(state.log_subscriber_id);
    }
}

void WebSocketServer::handle_writable(const std::string& connection_id, struct lws* wsi) {
    std::string msg;
    bool has_more = false;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = message_queues_.find(connection_id);
        if (it == message_queues_.end() || it->second.empty()) {
            return;
        }
        msg = std::move(it->second.front());
        it->second.pop();
        has_more = !it->second.empty();
    }

    std::vector<unsigned char> buf(LWS_PRE + msg.size());
    std::memcpy(&buf[LWS_PRE], msg.data(), msg.size());

    int written = lws_write(wsi, &buf[LWS_PRE], msg.size(), LWS_WRITE_TEXT);
    if (written < static_cast<int>(msg.size())) {
        LOG(ERROR, "WebSocket") << "Error writing to connection " << connection_id << std::endl;
        return;
    }

    if (has_more) {
        lws_callback_on_writable(wsi);
    }
}

std::optional<std::string> WebSocketServer::get_url_arg(struct lws* wsi, const char* name) {
    char buffer[512] = {0};
    const int value_len = lws_get_urlarg_by_name_safe(wsi, name, buffer, sizeof(buffer));
    if (value_len < 0) {
        return std::nullopt;
    }

    return std::string(buffer, static_cast<size_t>(value_len));
}

std::unordered_map<std::string, std::string> WebSocketServer::extract_params(
    struct lws* wsi,
    ConnectionKind kind) {
    std::unordered_map<std::string, std::string> params;

    if (kind == ConnectionKind::realtime) {
        if (auto model = get_url_arg(wsi, "model")) {
            params["model"] = *model;
        }
    } else if (kind == ConnectionKind::logs) {
        if (auto ticket = get_url_arg(wsi, "ticket")) {
            params["ticket"] = *ticket;
        }
    }

    return params;
}

std::string WebSocketServer::get_request_path(struct lws* wsi) {
    char uri_buf[256] = {0};

    lws_hdr_copy(wsi, uri_buf, sizeof(uri_buf), WSI_TOKEN_GET_URI);
    return std::string(uri_buf);
}

WebSocketServer::ConnectionKind WebSocketServer::classify_path(const std::string& path) {
    if (path == "/realtime") {
        return ConnectionKind::realtime;
    }
    if (path == "/logs/stream") {
        return ConnectionKind::logs;
    }
    return ConnectionKind::invalid;
}

void WebSocketServer::send_json(const std::string& connection_id, const json& msg) {
    std::string payload;
    try {
        payload = msg.dump(-1, ' ', false, json::error_handler_t::replace);

        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_websockets_.find(connection_id);
        if (it != connection_websockets_.end() && it->second != nullptr) {
            message_queues_[connection_id].push(std::move(payload));
            writable_dispatch_pending_.store(true);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "WebSocket send_json failed for %s: %s\n",
                     connection_id.c_str(), e.what());
    }

    if (context_) {
        lws_cancel_service(context_);
    }
}

void WebSocketServer::schedule_pending_writes() {
    if (!writable_dispatch_pending_.exchange(false)) {
        return;
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& [connection_id, wsi] : connection_websockets_) {
        if (wsi == nullptr) {
            continue;
        }

        auto queue_it = message_queues_.find(connection_id);
        if (queue_it != message_queues_.end() && !queue_it->second.empty()) {
            lws_callback_on_writable(wsi);
        }
    }
}

std::optional<std::optional<uint64_t>> WebSocketServer::consume_log_ticket(const std::string& ticket) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    cleanup_expired_tickets_locked();

    auto it = log_tickets_.find(ticket);
    if (it == log_tickets_.end()) {
        return std::nullopt;
    }

    auto after_seq = it->second.after_seq;
    log_tickets_.erase(it);
    return after_seq;
}

void WebSocketServer::cleanup_expired_tickets_locked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = log_tickets_.begin(); it != log_tickets_.end();) {
        if (it->second.expires_at <= now) {
            it = log_tickets_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace lemon
