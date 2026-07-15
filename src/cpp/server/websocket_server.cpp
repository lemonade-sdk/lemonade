#include "lemon/router.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/websocket_server.h"
#include "telemetry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

static struct lws_protocols protocols[] = {
    {"lemonade-realtime", WebSocketServer::ws_callback, sizeof(PerSessionData), 65536, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

} // namespace

WebSocketServer::WebSocketServer(Router* router, const std::string& host, int requested_port)
    : port_(requested_port > 0 ? requested_port : utils::ProcessManager::find_free_port(9000)),
      host_(host),
      api_key_([]() {
          const char* api_key_env = std::getenv("LEMONADE_API_KEY");
          return api_key_env ? std::string(api_key_env) : std::string();
      }()),
      admin_api_key_([this]() {
          const char* admin_key_env = std::getenv("LEMONADE_ADMIN_API_KEY");
          if (admin_key_env) {
              return std::string(admin_key_env);
          }
          return api_key_;
      }()),
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
            auto origin_opt = get_header(wsi, WSI_TOKEN_ORIGIN);
            if (origin_opt && !origin_opt->empty()) {
                std::string origin = *origin_opt;
                std::string origin_host = origin;
                size_t scheme_end = origin.find("://");
                if (scheme_end != std::string::npos) {
                    origin_host = origin.substr(scheme_end + 3);
                }
                size_t port_pos = std::string::npos;
                if (!origin_host.empty() && origin_host[0] == '[') {
                    size_t bracket_end = origin_host.find(']');
                    if (bracket_end != std::string::npos) {
                        port_pos = origin_host.find(':', bracket_end);
                    }
                } else {
                    port_pos = origin_host.find(':');
                }
                if (port_pos != std::string::npos) {
                    origin_host = origin_host.substr(0, port_pos);
                }
                size_t path_pos = origin_host.find('/');
                if (path_pos != std::string::npos) {
                    origin_host = origin_host.substr(0, path_pos);
                }

                std::string host_header_val;
                auto host_opt = get_header(wsi, WSI_TOKEN_HOST);
                if (host_opt && !host_opt->empty()) {
                    host_header_val = *host_opt;
                }
                std::string request_host = host_header_val;
                size_t r_port_pos = std::string::npos;
                if (!request_host.empty() && request_host[0] == '[') {
                    size_t bracket_end = request_host.find(']');
                    if (bracket_end != std::string::npos) {
                        r_port_pos = request_host.find(':', bracket_end);
                    }
                } else {
                    r_port_pos = request_host.find(':');
                }
                if (r_port_pos != std::string::npos) {
                    request_host = request_host.substr(0, r_port_pos);
                }
                size_t r_path_pos = request_host.find('/');
                if (r_path_pos != std::string::npos) {
                    request_host = request_host.substr(0, r_path_pos);
                }

                bool is_allowed = (origin_host == "localhost" || origin_host == "127.0.0.1" || origin_host == "[::1]" || origin_host == "::1");
                if (!is_allowed && !request_host.empty() && origin_host == request_host) {
                    is_allowed = true;
                }

                if (!is_allowed) {
                    LOG(WARNING, "WebSocket") << "Rejected connection from unauthorized origin: " << origin << std::endl;
                    return 1;
                }
            }

            const std::string path = get_request_path(wsi);
            if (classify_path(path) == ConnectionKind::invalid) {
                return 1;
            }
            if (!server->authenticate_connection(wsi)) {
                return 1;
            }
            break;
        }

        case LWS_CALLBACK_ESTABLISHED: {
            std::snprintf(pss->connection_id, sizeof(pss->connection_id), "%d", static_cast<int>(lws_get_socket_fd(wsi)));

            char ip[128] = {0};
            lws_get_peer_simple(wsi, ip, sizeof(ip));
            std::string ip_str(ip);
            if (ip_str.find("ENOTCONN") != std::string::npos || ip_str.empty()) {
                LOG(DEBUG, "WebSocket") << "Transient connection closed before handshake (id: "
                                       << pss->connection_id << ")" << std::endl;
            } else {
                LOG(DEBUG, "WebSocket") << "New connection from: " << ip
                                       << " (id: " << pss->connection_id << ")" << std::endl;
            }

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
                if (state_it == server->connection_states_.end()) {
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
    // Explicit vhost so we hold the vhost pointer for adopting upgrade
    // sockets handed over by the main HTTP server (see adopt_socket()).
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;

    if (host_.empty() || host_ == "localhost") {
        info.iface = "127.0.0.1";
    } else if (host_ != "0.0.0.0") {
        info.iface = host_.c_str();
    }

    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

    context_ = lws_create_context(&info);
    if (!context_) {
        LOG(ERROR, "WebSocket") << "Failed to create context on port " << port_ << std::endl;
        return false;
    }

    vhost_ = lws_create_vhost(context_, &info);
    if (!vhost_) {
        LOG(ERROR, "WebSocket") << "Failed to create vhost on port " << port_ << std::endl;
        lws_context_destroy(context_);
        context_ = nullptr;
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

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        telemetry_listener_registered_ = false;
    }
    telemetry::unregister_span_listener();

    running_.store(false);

    struct lws_context* ctx_to_destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (context_) {
            lws_cancel_service(context_);
            ctx_to_destroy = context_;
            context_ = nullptr;
            vhost_ = nullptr;
        }
    }

    if (service_thread_.joinable()) {
        service_thread_.join();
    }

    if (ctx_to_destroy) {
        lws_context_destroy(ctx_to_destroy);
    }

    // Snapshot and clear under the lock, then close sessions outside it:
    // closing a streaming session joins the backend TCP read thread, which
    // may concurrently be forwarding an event through send_json() — that
    // path takes connections_mutex_ (same pattern as handle_close()).
    std::unordered_map<std::string, ConnectionState> states;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        states = std::move(connection_states_);
        connection_states_.clear();
        connection_websockets_.clear();
        message_queues_.clear();
        receive_buffers_.clear();
    }
    for (const auto& [_, state] : states) {
        if (!state.realtime_session_id.empty()) {
            session_manager_->close_session(state.realtime_session_id);
        }
        if (!state.log_subscriber_id.empty()) {
            LogStreamHub::instance().remove_subscriber(state.log_subscriber_id);
        }
    }



    // Close any sockets that were queued for adoption but never picked up
    {
        std::lock_guard<std::mutex> lock(adoption_mutex_);
        while (!pending_adoptions_.empty()) {
            intptr_t fd = pending_adoptions_.front();
            pending_adoptions_.pop();
#ifdef _WIN32
            closesocket(static_cast<SOCKET>(fd));
#else
            ::close(static_cast<int>(fd));
#endif
        }
    }

    LOG(INFO, "WebSocket") << "Server stopped" << std::endl;
}

bool WebSocketServer::adopt_socket(intptr_t fd) {
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (!running_.load() || !context_) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(adoption_mutex_);
        pending_adoptions_.push(fd);
    }
    // Wake the service thread; adoption happens there (lws is not thread-safe)
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (context_) {
            lws_cancel_service(context_);
        }
    }
    return true;
}

void WebSocketServer::drain_pending_adoptions() {
    std::queue<intptr_t> pending;
    {
        std::lock_guard<std::mutex> lock(adoption_mutex_);
        std::swap(pending, pending_adoptions_);
    }

    while (!pending.empty()) {
        intptr_t fd = pending.front();
        pending.pop();

        lws_sock_file_fd_type desc;
        desc.sockfd = static_cast<lws_sockfd_type>(fd);
        struct lws* wsi = lws_adopt_descriptor_vhost(
            vhost_,
            static_cast<lws_adoption_type>(LWS_ADOPT_SOCKET | LWS_ADOPT_HTTP),
            desc, nullptr, nullptr);
        if (!wsi) {
            LOG(WARNING, "WebSocket") << "Failed to adopt upgraded socket" << std::endl;
#ifdef _WIN32
            closesocket(static_cast<SOCKET>(fd));
#else
            ::close(static_cast<int>(fd));
#endif
        }
    }
}

void WebSocketServer::service_loop() {
    while (running_.load()) {
        lws_service(context_, 50);
        drain_pending_adoptions();
        schedule_pending_writes();
    }
}

std::string WebSocketServer::strip_bearer_prefix(const std::string& token) const {
    static constexpr char bearer_prefix[] = "Bearer ";
    if (token.compare(0, sizeof(bearer_prefix) - 1, bearer_prefix) == 0) {
        return token.substr(sizeof(bearer_prefix) - 1);
    }
    return token;
}

std::string WebSocketServer::extract_token_from_wsi(struct lws* wsi) const {
    auto token = get_header(wsi, WSI_TOKEN_HTTP_AUTHORIZATION);
    if (!token) {
        token = get_url_arg(wsi, "api_key");
    }
    if (!token) {
        token = get_protocol_credential(wsi);
    }
    return token ? strip_bearer_prefix(*token) : "";
}

bool WebSocketServer::authenticate_connection(struct lws* wsi) const {
    if (api_key_.empty()) {
        return true;
    }

    auto token = get_header(wsi, WSI_TOKEN_HTTP_AUTHORIZATION);
    if (!token) {
        token = get_url_arg(wsi, "api_key");
    }
    if (!token) {
        token = get_protocol_credential(wsi);
    }

    if (!token) {
        std::string path = get_request_path(wsi);
        ConnectionKind kind = classify_path(path);
        if (kind == ConnectionKind::spans) {
            return true;
        }
        LOG(WARNING, "WebSocket") << "Rejected upgrade for path: " << path << " due to missing credentials" << std::endl;
        return false;
    }

    std::string token_str = strip_bearer_prefix(*token);

    if ((token_str != api_key_) && (admin_api_key_.empty() || (token_str != admin_api_key_))) {
        LOG(WARNING, "WebSocket") << "Rejected websocket connection with invalid API key for "
                                  << get_request_path(wsi) << std::endl;
        return false;
    }

    return true;
}

void WebSocketServer::handle_connection(const std::string& connection_id, struct lws* wsi) {
    const std::string path = get_request_path(wsi);
    const auto kind = classify_path(path);

    std::string token_str = extract_token_from_wsi(wsi);
    auto client_session_id_opt = get_url_arg(wsi, "client_session_id");
    std::string client_session_id = client_session_id_opt ? *client_session_id_opt : "";

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_websockets_[connection_id] = wsi;
        ConnectionState state;
        state.kind = kind;
        state.authenticated_token = token_str;
        state.authenticated_token_hash = telemetry::hash_token(token_str);
        state.client_session_id = client_session_id;
        state.authenticated = api_key_.empty() || (!token_str.empty() && (token_str == api_key_ || token_str == admin_api_key_));
        connection_states_[connection_id] = state;
    }

    if (kind == ConnectionKind::realtime) {
        handle_realtime_connection(connection_id, wsi);
    }
    // Logs connections wait for a "logs.subscribe" message before streaming.

    update_telemetry_listener_registration();
}

void WebSocketServer::handle_realtime_connection(
    const std::string& connection_id,
    struct lws* wsi) {
    json initial_config = json::object();
    if (auto model = get_url_arg(wsi, "model")) {
        initial_config["model"] = *model;
    }

    auto send_callback = [this, connection_id](const json& msg) {
        send_json(connection_id, msg);
    };

    const std::string session_id = session_manager_->create_session(send_callback, initial_config);

    std::lock_guard<std::mutex> lock(connections_mutex_);
    connection_states_[connection_id].realtime_session_id = session_id;
}

void WebSocketServer::handle_log_subscribe(const std::string& connection_id,
                                           std::optional<uint64_t> after_seq) {
    std::vector<LogStreamEntry> snapshot_entries;
    const std::string subscriber_id = LogStreamHub::instance().subscribe_with_snapshot(
        [this, connection_id](const LogStreamEntry& entry) {
            send_json(connection_id, {
                {"type", "logs.entry"},
                {"entry", entry.to_json()},
            });
        },
        after_seq,
        snapshot_entries);

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
    ConnectionKind kind;
    std::string session_id;
    bool is_authenticated = false;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connection_states_.find(connection_id);
        if (it == connection_states_.end()) {
            return;
        }
        kind = it->second.kind;
        session_id = it->second.realtime_session_id;
        is_authenticated = it->second.authenticated;
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

    if (!is_authenticated) {
        if (msg_type == "auth") {
            std::string token = request.value("token", "");
            if (token.empty()) {
                token = request.value("api_key", "");
            }
            token = strip_bearer_prefix(token);

            bool is_valid = false;
            if (api_key_.empty()) {
                is_valid = true;
            } else if (token == api_key_ || (!admin_api_key_.empty() && token == admin_api_key_)) {
                is_valid = true;
            }

            if (is_valid) {
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    auto state_it = connection_states_.find(connection_id);
                    if (state_it != connection_states_.end()) {
                        state_it->second.authenticated_token = token;
                        state_it->second.authenticated_token_hash = telemetry::hash_token(token);
                        state_it->second.authenticated = true;
                        if (request.contains("client_session_id")) {
                            state_it->second.client_session_id = request.value("client_session_id", "");
                        }
                    }
                }
                send_json(connection_id, {{"type", "auth.ok"}});
                update_telemetry_listener_registration();
            } else {
                send_json(connection_id, {
                    {"type", "error"},
                    {"error", {{"message", "Invalid API key"}, {"type", "invalid_request_error"}}}
                });
            }
        } else {
            send_json(connection_id, {
                {"type", "error"},
                {"error", {{"message", "Unauthorized. Please authenticate first by sending an auth message."}, {"type", "invalid_request_error"}}},
            });
        }
        return;
    }

    if (msg_type == "auth") {
        std::string token = request.value("token", "");
        if (token.empty()) {
            token = request.value("api_key", "");
        }
        token = strip_bearer_prefix(token);

        bool is_valid = false;
        if (api_key_.empty()) {
            is_valid = true;
        } else if (token == api_key_ || (!admin_api_key_.empty() && token == admin_api_key_)) {
            is_valid = true;
        }

        if (is_valid) {
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto state_it = connection_states_.find(connection_id);
                if (state_it != connection_states_.end()) {
                    state_it->second.authenticated_token = token;
                    state_it->second.authenticated_token_hash = telemetry::hash_token(token);
                    state_it->second.authenticated = true;
                    if (request.contains("client_session_id")) {
                        state_it->second.client_session_id = request.value("client_session_id", "");
                    }
                }
            }
            send_json(connection_id, {{"type", "auth.ok"}});
            update_telemetry_listener_registration();
        } else {
            send_json(connection_id, {
                {"type", "error"},
                {"error", {{"message", "Invalid API key"}, {"type", "invalid_request_error"}}}
            });
        }
        return;
    }

    if (kind == ConnectionKind::logs) {
        if (msg_type == "logs.subscribe") {
            std::optional<uint64_t> after_seq;
            if (request.contains("after_seq") && !request["after_seq"].is_null()) {
                after_seq = request["after_seq"].get<uint64_t>();
            }
            handle_log_subscribe(connection_id, after_seq);
        } else {
            send_json(connection_id, {
                {"type", "error"},
                {"error", {{"message", "Expected logs.subscribe message"}, {"type", "invalid_request_error"}}},
            });
        }
        return;
    }

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

    update_telemetry_listener_registration();
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

std::optional<std::string> WebSocketServer::get_header(struct lws* wsi, enum lws_token_indexes token) {
    char buffer[512] = {0};
    const int copied = lws_hdr_copy(wsi, buffer, sizeof(buffer), token);
    if (copied <= 0) {
        return std::nullopt;
    }

    return std::string(buffer, static_cast<size_t>(copied));
}

std::optional<std::string> WebSocketServer::get_protocol_credential(struct lws* wsi) {
    static constexpr char credential_prefix[] = "bearer.";
    static constexpr size_t prefix_len = sizeof(credential_prefix) - 1;

    auto header = get_header(wsi, WSI_TOKEN_PROTOCOL);
    if (!header) {
        return std::nullopt;
    }

    // The credential rides in Sec-WebSocket-Protocol alongside the registered
    // application protocol. The encoding is base64url, so the value contains
    // only token characters and ends at the next list separator.
    const std::string& value = *header;
    size_t pos = value.find(credential_prefix);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    size_t begin = pos + prefix_len;
    size_t end = value.find_first_of(", \t", begin);
    std::string encoded = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);

    // Translate base64url back to standard base64 before decoding.
    for (char& c : encoded) {
        if (c == '-') {
            c = '+';
        } else if (c == '_') {
            c = '/';
        }
    }
    return utils::JsonUtils::base64_decode(encoded);
}

std::optional<std::string> WebSocketServer::get_url_arg(struct lws* wsi, const char* name) {
    char buffer[512] = {0};
    const int value_len = lws_get_urlarg_by_name_safe(wsi, name, buffer, sizeof(buffer));
    if (value_len < 0) {
        return std::nullopt;
    }

    return std::string(buffer, static_cast<size_t>(value_len));
}

std::string WebSocketServer::get_request_path(struct lws* wsi) {
    char uri_buf[256] = {0};

    lws_hdr_copy(wsi, uri_buf, sizeof(uri_buf), WSI_TOKEN_GET_URI);
    return std::string(uri_buf);
}

WebSocketServer::ConnectionKind WebSocketServer::classify_path(const std::string& path) {
    // Quad-prefix invariant: endpoints are reachable bare and under
    // /api/v0, /api/v1, /v0, /v1. OpenAI Realtime SDK clients connect
    // to /v1/realtime.
    std::string stripped = path;
    for (const char* prefix : {"/api/v0", "/api/v1", "/v0", "/v1"}) {
        size_t len = std::strlen(prefix);
        if (stripped.rfind(prefix, 0) == 0 && stripped.size() > len && stripped[len] == '/') {
            stripped = stripped.substr(len);
            break;
        }
    }
    if (stripped == "/realtime") {
        return ConnectionKind::realtime;
    }
    if (stripped == "/logs/stream") {
        return ConnectionKind::logs;
    }
    if (stripped == "/spans/stream" || stripped == "/traces/stream") {
        return ConnectionKind::spans;
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

    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (context_) {
            lws_cancel_service(context_);
        }
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

void WebSocketServer::update_telemetry_listener_registration() {
    bool has_active_spans_conn = false;
    bool should_register = false;
    bool should_unregister = false;

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [conn_id, state] : connection_states_) {
            if (state.kind == ConnectionKind::spans && state.authenticated) {
                has_active_spans_conn = true;
                break;
            }
        }

        if (has_active_spans_conn && !telemetry_listener_registered_) {
            telemetry_listener_registered_ = true;
            should_register = true;
        } else if (!has_active_spans_conn && telemetry_listener_registered_) {
            telemetry_listener_registered_ = false;
            should_unregister = true;
        }
    }

    if (should_register) {
        telemetry::register_span_listener([this](const json& span) {
            this->broadcast_span(span);
        });
    } else if (should_unregister) {
        telemetry::unregister_span_listener();
    }
}

void WebSocketServer::broadcast_span(const json& span) {
    std::string target_token_hash;
    std::string span_session_id;
    if (span.contains("attributes") && span["attributes"].is_array()) {
        for (const auto& attr : span["attributes"]) {
            if (attr.is_object()) {
                std::string key = attr.value("key", "");
                if (key == "lemon.auth_token_hash") {
                    if (attr.contains("value") && attr["value"].contains("stringValue")) {
                        target_token_hash = attr["value"]["stringValue"].get<std::string>();
                    }
                } else if (key == "lemon.client_session_id") {
                    if (attr.contains("value") && attr["value"].contains("stringValue")) {
                        span_session_id = attr["value"]["stringValue"].get<std::string>();
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& [conn_id, state] : connection_states_) {
        if (state.kind == ConnectionKind::spans) {
            if (!state.authenticated) {
                continue;
            }

            bool is_admin = !admin_api_key_.empty() && (state.authenticated_token == admin_api_key_);
            bool matches_token = !target_token_hash.empty() && (state.authenticated_token_hash == target_token_hash);
            bool guest_allowed = target_token_hash.empty() && api_key_.empty();

            if (is_admin || matches_token || guest_allowed) {
                bool is_guest = !is_admin && !matches_token;
                if (is_guest) {
                    if (state.client_session_id.empty() || span_session_id.empty() || state.client_session_id != span_session_id) {
                        continue;
                    }
                }

                try {
                    // Deep clone to strip sensitive auth token hash from broadcast
                    json filtered_span = span;
                    if (filtered_span.contains("attributes") && filtered_span["attributes"].is_array()) {
                        auto& attrs = filtered_span["attributes"];
                        for (auto it = attrs.begin(); it != attrs.end(); ) {
                            if (it->is_object() && it->value("key", "") == "lemon.auth_token_hash") {
                                it = attrs.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }

                    std::string payload = filtered_span.dump(-1, ' ', false, json::error_handler_t::replace);
                    auto it = connection_websockets_.find(conn_id);
                    if (it != connection_websockets_.end() && it->second != nullptr) {
                        message_queues_[conn_id].push(std::move(payload));
                        writable_dispatch_pending_.store(true);
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "WebSocket broadcast_span failed for %s: %s\n",
                                 conn_id.c_str(), e.what());
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (context_) {
            lws_cancel_service(context_);
        }
    }
}

bool WebSocketServer::has_span_listeners() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& [conn_id, state] : connection_states_) {
        if (state.kind == ConnectionKind::spans) {
            return true;
        }
    }
    return false;
}

bool WebSocketServer::is_websocket_path(const std::string& path) {
    return classify_path(path) != ConnectionKind::invalid;
}

bool is_websocket_endpoint(const std::string& path) {
    return WebSocketServer::is_websocket_path(path);
}

} // namespace lemon
