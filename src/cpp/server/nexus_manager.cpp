#include "lemon/nexus_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>
#endif

namespace lemon {
namespace {
bool local_request(const httplib::Request& req) {
    return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" || req.remote_addr == "::ffff:127.0.0.1";
}
}
#ifdef LEMONADE_WITH_NEXUS
namespace {
class NexusError : public std::runtime_error {
public:
    NexusError(int status, const std::string& message) : std::runtime_error(message), status(status) {}
    int status;
};
using json = nlohmann::json;
json sdk_call(const std::function<ln_error_t(char**)>& operation) {
    char* output = nullptr;
    const auto code = operation(&output);
    std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
    auto value = output ? json::parse(output, nullptr, false) : json::object();
    if (value.is_discarded()) throw NexusError(502, "The network returned invalid data");
    if (code != LN_OK) {
        throw NexusError(code == LN_ERR_AUTH ? 403 : code == LN_ERR_NOT_FOUND ? 404 : 502,
            value.value("error", std::string("Network request failed (SDK error ") + std::to_string(code) + ")"));
    }
    return value;
}
std::string sdk_string(char* value) {
    std::unique_ptr<char, decltype(&ln_free)> owned(value, ln_free);
    return value ? value : "";
}
}
struct NexusManager::Session {
    std::unique_ptr<ln_client_t, decltype(&ln_destroy)> client{nullptr, ln_destroy};
    std::unique_ptr<ln_identity_t, decltype(&ln_identity_destroy)> identity{nullptr, ln_identity_destroy};
    std::filesystem::path root;
    int server_port;
    json account = json::object();
    bool locked{true};
    bool joined{false};
    bool reconnect{false};
    std::chrono::steady_clock::time_point retry_at{};
    std::chrono::seconds retry_delay{5};
    std::map<std::string, std::string> egress_ips;
    std::string node_id;
    std::string group_id;
    std::string tunnel_ip;
    std::string host{"lemonade-nexus.io"};
    std::string rp_id{"lemonade-nexus.io"};
    std::string pubkey;
    std::string detail{"locked"};
    std::string error;
    json devices = json::array();
    std::chrono::steady_clock::time_point confirmed_at{};
    std::chrono::system_clock::time_point last_poll{std::chrono::system_clock::now()};

    Session(const std::string& data_root, int port) : root(utils::path_from_utf8(data_root)), server_port(port) {
        std::filesystem::create_directories(root);
#ifndef _WIN32
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);
#endif
        std::ifstream input(root / "account.json");
        if (input) {
            auto saved = json::parse(input, nullptr, false);
            if (!saved.is_object()) throw NexusError(500, "Device account is invalid; restore account.json");
            account = saved;
        }
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_SERVER"); !value.empty()) host = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_RP_ID"); !value.empty()) rp_id = value;
        int public_port = 9100;
        const auto configured_port = utils::get_environment_variable_utf8("LEMONADE_NEXUS_PORT");
        if (!configured_port.empty()) public_port = std::stoi(configured_port);
        if (public_port < 1 || public_port > 65535) throw NexusError(400, "Invalid Nexus server port");
        client.reset(ln_create_tls(host.c_str(), static_cast<uint16_t>(public_port)));
        if (!client) throw NexusError(500, "Cannot create Nexus client");
        const auto path = utils::path_to_utf8(root / "identity.json");
        identity.reset(ln_identity_load(path.c_str()));
        if (!identity) {
            if (std::filesystem::exists(root / "identity.json")) throw NexusError(500, "Device identity is invalid; restore identity.json");
            identity.reset(ln_identity_generate());
            if (!identity || ln_identity_save(identity.get(), path.c_str()) != LN_OK) throw NexusError(500, "Cannot save device identity");
        }
        if (ln_set_identity(client.get(), identity.get()) != LN_OK) throw NexusError(500, "Cannot load device identity");
        pubkey = sdk_string(ln_identity_pubkey(identity.get()));
    }
    ~Session() { if (client) ln_mesh_disable(client.get()); }
    std::string device_name() const { return account.value("device_name", "My Lemonade device"); }
    void persist() {
        const auto temporary = root / "account.json.tmp";
        std::ofstream output(temporary);
        output << account.dump(2);
        output.close();
        if (!output) throw NexusError(500, "Cannot save device account");
        std::error_code ec;
        if (!utils::atomic_replace_file(temporary, root / "account.json", ec)) throw NexusError(500, "Cannot replace device account");
    }
    void lock() {
        locked = true;
        joined = false;
        reconnect = false;
        egress_ips.clear();
        ln_mesh_disable(client.get());
        ln_set_session_token(client.get(), "");
        ln_set_link_token(client.get(), "");
        devices = json::array();
        detail = "locked";
        error.clear();
    }
    json authenticate_identity() {
        auto result = sdk_call([&](auto out) { return ln_auth_ed25519(client.get(), out); });
        if (account.contains("user_id") && account.at("user_id") != result.at("user_id")) {
            throw NexusError(403, "Account does not belong to this device identity");
        }
        account["user_id"] = result.at("user_id");
        return result;
    }
    void refresh_devices() {
        const auto own = sdk_call([&](auto out) { return ln_tree_get_node(client.get(), node_id.c_str(), out); });
        group_id = own.at("parent_id").get<std::string>();
        const auto members = sdk_call([&](auto out) { return ln_get_group_members(client.get(), group_id.c_str(), out); });
        const auto children = sdk_call([&](auto out) { return ln_tree_get_children(client.get(), group_id.c_str(), out); });
        const auto peers = sdk_call([&](auto out) { return ln_mesh_peers(client.get(), out); });
        auto assignment = std::find_if(members.begin(), members.end(), [&](const json& member) {
            return member.value("management_pubkey", "") == pubkey;
        });
        bool owner = false;
        if (assignment != members.end()) {
            const auto permissions = assignment->value("permissions", std::vector<std::string>{});
            owner = std::find(permissions.begin(), permissions.end(), "delete_node") != permissions.end();
        }
        json next = json::array();
        for (const auto& child : children) {
            if (child.value("type", "") != "endpoint") continue;
            auto member = std::find_if(members.begin(), members.end(), [&](const json& m) {
                return m.value("management_pubkey", "") == child.value("mgmt_pubkey", "");
            });
            const auto id = child.at("id").get<std::string>();
            auto peer = std::find_if(peers.begin(), peers.end(), [&](const json& p) { return p.value("node_id", "") == id; });
            next.push_back({{"node_id", id}, {"device_name", child.value("hostname", "")},
                {"tunnel_ip", child.value("tunnel_ip", "")}, {"management_pubkey", child.value("mgmt_pubkey", "")},
                {"permissions", member == members.end() ? json::array() : member->value("permissions", json::array())}, {"can_remove", owner && id != node_id},
                {"is_online", id == node_id || (peer != peers.end() && peer->value("is_online", false))},
                {"latency_ms", peer == peers.end() ? 0.0 : peer->value("latency_ms", 0.0)}});
        }
        for (auto it = egress_ips.begin(); it != egress_ips.end();) {
            const auto present = std::find_if(next.begin(), next.end(), [&](const json& device) {
                auto ip = device.value("tunnel_ip", "");
                if (const auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
                return device.value("node_id", "") == it->first && ip == it->second;
            });
            if (present == next.end()) {
                ln_mesh_close_egress(client.get(), it->second.c_str(), 13305);
                it = egress_ips.erase(it);
            } else ++it;
        }
        devices = std::move(next);
    }
    json status() {
        auto mesh = sdk_call([&](auto out) { return ln_mesh_status(client.get(), out); });
        return {{"status", detail}, {"locked", locked}, {"mesh_up", !locked && ln_mesh_is_active(client.get()) != 0},
            {"node_id", node_id}, {"group_node_id", group_id}, {"tunnel_ip", tunnel_ip},
            {"peer_count", mesh.value("peer_count", 0)}, {"device_name", device_name()}, {"server", host},
            {"registered", account.contains("credential_id")}, {"members", devices},
            {"exposed", json::array({{{"vport", 13305}, {"local_port", server_port}}})}, {"error", error}};
    }
    void join(const std::string& code) {
        if (locked) throw NexusError(423, "Unlock with your passkey before joining");
        detail = "joining";
        ln_set_link_token(client.get(), code.c_str());
        json joined_result;
        try { joined_result = sdk_call([&](auto out) { return ln_join_network(client.get(), device_name().c_str(), "", out); }); }
        catch (const std::exception& e) {
            ln_set_link_token(client.get(), "");
            detail = "degraded";
            error = e.what();
            throw;
        }
        ln_set_link_token(client.get(), "");
        node_id = joined_result.at("node_id").get<std::string>();
        tunnel_ip = joined_result.at("tunnel_ip").get<std::string>();
        if (ln_mesh_enable(client.get()) != LN_OK || ln_mesh_expose_service(client.get(), 13305,
                ("tcp:127.0.0.1:" + std::to_string(server_port)).c_str()) != LN_OK) {
            lock();
            throw NexusError(502, "Could not start the mesh transport");
        }
        joined = true;
        reconnect = true;
        retry_delay = std::chrono::seconds(5);
        detail = "running";
        error.clear();
        try {
            const auto updates = json{{"hostname", device_name()}}.dump();
            sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
            refresh_devices();
        } catch (const std::exception& e) { error = e.what(); }
    }
    void poll() {
        const auto now = std::chrono::system_clock::now();
        const auto elapsed = now - last_poll;
        last_poll = now;
        if (!locked && elapsed > std::chrono::minutes(5)) { lock(); return; }
        if (locked || !reconnect || std::chrono::steady_clock::now() < retry_at) return;
        try {
            if (!joined || !ln_mesh_is_active(client.get())) {
                ln_mesh_disable(client.get());
                egress_ips.clear();
                joined = false;
                join("");
            }
            else { refresh_devices(); error.clear(); }
        } catch (const std::exception& e) {
            error = e.what(); detail = "degraded";
            retry_at = std::chrono::steady_clock::now() + retry_delay;
            retry_delay = std::min(retry_delay * 2, std::chrono::seconds(60));
        }
    }
    json request(const std::string& path, const json& body) {
        if (path == "/status") return status();
        if (path == "/device") {
            const auto name = body.at("device_name").get<std::string>();
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter a device name (up to 128 characters)");
            if (joined && !locked) {
                const auto updates = json{{"hostname", name}}.dump();
                sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
            }
            account["device_name"] = name;
            persist();
            return status();
        }
        if (path.rfind("/passkey/challenge", 0) == 0) {
            const auto authenticated = authenticate_identity();
            const auto user_id = authenticated.at("user_id").get<std::string>();
            const auto challenge = sdk_call([&](auto out) { return ln_auth_passkey_challenge(client.get(), user_id.c_str(), out); });
            return {{"challenge", challenge.at("challenge")}, {"user_id", user_id}, {"rp_id", rp_id},
                {"credential_id", account.value("credential_id", "")}, {"registration", path.find("register=1") != std::string::npos}};
        }
        if (path == "/passkey/register") {
            if (account.contains("credential_id")) throw NexusError(409, "This device already has a passkey");
            if (!account.contains("user_id")) throw NexusError(409, "Request a registration challenge first");
            const auto authenticated = authenticate_identity();
            const auto user_id = authenticated.at("user_id").get<std::string>();
            const auto credential_id = body.at("credential_id").get<std::string>();
            const auto x = body.at("public_key_x").get<std::string>();
            const auto y = body.at("public_key_y").get<std::string>();
            sdk_call([&](auto out) { return ln_register_passkey(client.get(), user_id.c_str(), credential_id.c_str(), x.c_str(), y.c_str(), out); });
            account["credential_id"] = credential_id;
            persist();
            return status();
        }
        if (path == "/unlock") {
            const auto& assertion = body.at("passkey_assertion");
            if (!account.contains("credential_id") || assertion.at("credential_id") != account.at("credential_id")) throw NexusError(403, "Use this device's registered passkey");
            auto encoded = assertion.at("authenticator_data").get<std::string>();
            std::replace(encoded.begin(), encoded.end(), '-', '+');
            std::replace(encoded.begin(), encoded.end(), '_', '/');
            while (encoded.size() % 4) encoded += '=';
            const auto bytes = utils::JsonUtils::base64_decode(encoded);
            if (bytes.size() < 37 || (static_cast<unsigned char>(bytes[32]) & 0x05) != 0x05) throw NexusError(403, "Passkey user verification is required");
            const auto payload = json{{"assertion", assertion}}.dump();
            const auto authenticated = sdk_call([&](auto out) { return ln_auth_passkey(client.get(), payload.c_str(), out); });
            if (authenticated.at("user_id") != account.at("user_id")) {
                ln_set_session_token(client.get(), "");
                throw NexusError(403, "Passkey does not match this device identity");
            }
            authenticate_identity();
            locked = false;
            confirmed_at = std::chrono::steady_clock::now();
            last_poll = std::chrono::system_clock::now();
            detail = joined ? "running" : "ready";
            if (!body.value("defer_join", false) && !joined) join("");
            return status();
        }
        if (path == "/lock") { lock(); return status(); }
        if (path == "/link-token/cancel") return {{"cancelled", true}, {"revoked", false}};
        if (locked) throw NexusError(423, "Unlock your network first");
        if (path == "/join") {
            const auto code = body.at("code").get<std::string>();
            const bool full_token = code.size() == 68 && code.rfind("lnk_", 0) == 0 && code.substr(4).find_first_not_of("0123456789abcdef") == std::string::npos;
            if (!full_token && (code.size() != 8 || code.find_first_not_of("0123456789") != std::string::npos)) throw NexusError(400, "Enter an 8-digit code or a full invite token");
            if (joined) throw NexusError(409, "Lock this device before joining another group");
            join(code);
            return status();
        }
        if (!joined) throw NexusError(409, "Join the network first");
        if (path == "/devices") { refresh_devices(); return devices; }
        if (path == "/link-token") {
            if (std::chrono::steady_clock::now() - confirmed_at > std::chrono::seconds(60)) throw NexusError(423, "Confirm with your passkey before inviting a device");
            const auto ttl = body.value("ttl_sec", 600);
            if (ttl < 60 || ttl > 600) throw NexusError(400, "Invite lifetime must be 60 to 600 seconds");
            const auto name = body.at("device_name").get<std::string>();
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter the name of the device you're inviting");
            auto result = sdk_call([&](auto out) { return ln_link_token_create(client.get(), ttl, out); });
            result["device_name"] = name;
            return result;
        }
        refresh_devices();
        const auto id = path == "/egress" ? body.at("node_id").get<std::string>() : path.substr(9, path.size() - 16);
        auto target = std::find_if(devices.begin(), devices.end(), [&](const json& d) { return d.at("node_id") == id; });
        if (target == devices.end() || id == node_id) throw NexusError(404, "Choose another member of your group");
        if (path == "/egress") {
            auto ip = target->at("tunnel_ip").get<std::string>();
            if (auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
            uint16_t port = 0;
            if (ln_mesh_open_egress(client.get(), ip.c_str(), 13305, &port) != LN_OK || !port) throw NexusError(502, "Remote server connection failed");
            egress_ips[id] = ip;
            return {{"node_id", id}, {"loopback_port", port}, {"endpoint", "http://127.0.0.1:" + std::to_string(port)}};
        }
        if (path.rfind("/devices/", 0) == 0 && path.size() > 16) {
            if (!target->at("can_remove").get<bool>()) throw NexusError(403, "Only a group owner can remove this device");
            const auto removed_key = target->at("management_pubkey").get<std::string>();
            sdk_call([&](auto out) { return ln_private_api_call(client.get(), "POST", ("/api/tree/node/delete/" + id).c_str(), "{}", out); });
            sdk_call([&](auto out) { return ln_remove_group_member(client.get(), group_id.c_str(), removed_key.c_str(), out); });
            refresh_devices();
            return {{"removed", true}};
        }
        throw NexusError(404, "Unknown network operation");
    }
};
#else
struct NexusManager::Session {};
#endif

NexusManager::NexusManager(const std::string& config_dir, int server_port)
    : data_root_(utils::path_to_utf8(utils::path_from_utf8(config_dir) / "nexus")), server_port_(server_port) {
#ifdef LEMONADE_WITH_NEXUS
    std::ifstream input(utils::path_from_utf8(data_root_) / "preferences.json");
    auto saved = json::parse(input, nullptr, false);
    if (saved.is_object() && saved.value("enabled", false)) {
        std::string error;
        enable(error);
    }
    worker_ = std::thread(&NexusManager::supervise, this);
#endif
    register_routes(management_server_, true);
    management_server_.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        const auto key = utils::get_environment_variable_utf8("LEMONADE_API_KEY");
        const auto admin_key = utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY");
        const auto origin = req.get_header_value("Origin");
        const bool allowed_origin = origin.empty() || origin == "tauri://localhost" || origin == "http://tauri.localhost" || origin == "https://tauri.localhost" ||
            origin.rfind("http://127.0.0.1:", 0) == 0 || origin.rfind("http://localhost:", 0) == 0;
        if (!local_request(req) || !allowed_origin) {
            res.status = 403; res.set_content("{\"error\":\"Local network management only\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!origin.empty()) { res.set_header("Access-Control-Allow-Origin", origin); res.set_header("Vary", "Origin"); }
        if (req.method == "OPTIONS") {
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!key.empty() && req.get_header_value("Authorization") != "Bearer " + key &&
            (admin_key.empty() || req.get_header_value("Authorization") != "Bearer " + admin_key)) {
            res.status = 401; res.set_content("{\"error\":\"API key required\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    management_port_ = management_server_.bind_to_any_port("127.0.0.1");
    if (management_port_ < 0) { stop(); throw std::runtime_error("Cannot bind local network management listener"); }
    management_thread_ = std::thread([this] { management_server_.listen_after_bind(); });
    management_server_.wait_until_ready();
}
NexusManager::~NexusManager() { stop(); }
void NexusManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wake_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    management_server_.stop();
    if (management_thread_.joinable()) management_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    session_.reset();
}
NexusManager::json NexusManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = snapshot_;
    result["enabled"] = enabled_;
    result["controller_url"] = "http://127.0.0.1:" + std::to_string(management_port_);
#ifdef LEMONADE_WITH_NEXUS
    result["available"] = true;
#else
    result["available"] = false;
#endif
    result["event_sequence"] = event_sequence_;
    if (!active_invite_.is_null()) result["active_invite"] = {{"device_name", active_invite_.value("device_name", "")}, {"expires_at", active_invite_.value("expires_at", uint64_t{0})}};
    return result;
}
void NexusManager::persist_enabled_locked() {
    const auto root = utils::path_from_utf8(data_root_);
    std::filesystem::create_directories(root);
    const auto temporary = root / "preferences.json.tmp";
    std::ofstream output(temporary);
    output << json{{"enabled", enabled_}}.dump();
    output.close();
    if (!output) throw std::runtime_error("Cannot save Nexus preferences");
    std::error_code ec;
    if (!utils::atomic_replace_file(temporary, root / "preferences.json", ec)) throw std::runtime_error("Cannot save Nexus preferences: " + ec.message());
}
bool NexusManager::enable(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
#ifndef LEMONADE_WITH_NEXUS
    error = "This build does not include the Nexus SDK";
    return false;
#else
    try {
        if (stopping_) throw std::runtime_error("Server is shutting down");
        if (!session_) session_ = std::make_unique<Session>(data_root_, server_port_);
        enabled_ = true;
        persist_enabled_locked();
        update_snapshot_locked(session_->status());
        return true;
    } catch (const std::exception& e) {
        enabled_ = false;
        session_.reset();
        error = e.what();
        snapshot_["error"] = error;
        return false;
    }
#endif
}
void NexusManager::disable() {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    session_.reset();
    active_invite_ = nullptr;
    update_snapshot_locked({{"status", "disabled"}, {"locked", true}, {"mesh_up", false}, {"members", json::array()}});
    persist_enabled_locked();
}
std::pair<int, NexusManager::json> NexusManager::control_locked(const std::string&, const std::string& path, const json& body) {
#ifdef LEMONADE_WITH_NEXUS
    if (!enabled_ || !session_) return {409, {{"error", "Enable mesh first"}}};
    std::pair<int, json> result;
    try { result = {200, session_->request(path, body)}; }
    catch (const NexusError& e) { result = {e.status, {{"error", e.what()}}}; }
    catch (const json::exception&) { result = {400, {{"error", "Invalid request fields"}}}; }
    catch (const std::exception& e) { result = {502, {{"error", e.what()}}}; }
    try { update_snapshot_locked(session_->status()); } catch (...) {}
    return result;
#else
    return {503, {{"error", "This build does not include the Nexus SDK"}}};
#endif
}

void NexusManager::emit_locked(const std::string& type, const json& data) {
    events_.push_back({{"sequence", ++event_sequence_}, {"type", type}, {"data", data}});
    while (events_.size() > 128) events_.pop_front();
    wake_.notify_all();
}

void NexusManager::update_snapshot_locked(const json& snapshot) {
    if (snapshot.value("locked", true) != snapshot_.value("locked", true)) {
        emit_locked("nexus.lock_changed", {{"locked", snapshot.value("locked", true)}});
    }
    if (snapshot.value("status", "") != snapshot_.value("status", "") ||
        snapshot.value("mesh_up", false) != snapshot_.value("mesh_up", false)) {
        emit_locked("nexus.mesh_changed", {{"status", snapshot.value("status", "")},
                                          {"mesh_up", snapshot.value("mesh_up", false)}});
    }
    const auto previous = snapshot_.value("members", json::array());
    const auto next = snapshot.value("members", json::array());
    if (snapshot.value("mesh_up", false) && snapshot_.value("mesh_up", false)) {
        for (const auto& member : next) {
            if (std::none_of(previous.begin(), previous.end(), [&](const json& old) {
                return old.value("node_id", "") == member.value("node_id", "");
            })) emit_locked("nexus.member_joined", member);
        }
        for (const auto& member : previous) {
            if (std::none_of(next.begin(), next.end(), [&](const json& now) {
                return now.value("node_id", "") == member.value("node_id", "");
            })) emit_locked("nexus.member_left", member);
        }
    }
    snapshot_ = snapshot;
}

void NexusManager::supervise() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
#ifdef LEMONADE_WITH_NEXUS
        if (enabled_ && session_) {
            try { session_->poll(); update_snapshot_locked(session_->status()); }
            catch (const std::exception& e) { snapshot_["error"] = e.what(); }
        }
#endif
        wake_.wait_for(lock, std::chrono::seconds(5), [this] { return stopping_; });
    }
}

void NexusManager::handle(const httplib::Request& req, httplib::Response& res,
                           const std::string& operation) {
    res.set_header("Cache-Control", "no-store");
    const auto reply = [&](int code, const json& body) {
        res.status = code;
        res.set_content(body.dump(), "application/json");
    };
    if (!local_request(req)) {
        reply(403, {{"error", "Manage mesh from the desktop app connected to this device's local server"}});
        return;
    }
    auto body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
    if (!body.is_object() || req.body.size() > 65536) {
        reply(400, {{"error", "Expected a JSON object of at most 64 KiB"}});
        return;
    }
    try {
        if (operation == "status" || operation == "join/status") { reply(200, status()); return; }
        if (operation == "enable") {
            std::string error;
            if (!enable(error)) { reply(503, {{"error", error}}); return; }
            reply(200, status());
            return;
        }
        if (operation == "disable") { disable(); reply(200, status()); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        if (operation == "events") {
            uint64_t after = 0;
            if (req.has_param("after")) after = std::stoull(req.get_param_value("after"));
            std::string stream;
            for (const auto& event : events_) {
                if (event.at("sequence").get<uint64_t>() <= after) continue;
                stream += "id: " + std::to_string(event.at("sequence").get<uint64_t>()) +
                    "\nevent: " + event.at("type").get<std::string>() + "\ndata: " + event.dump() + "\n\n";
            }
            if (req.has_param("once") && req.get_param_value("once") == "1") {
                res.set_content(stream.empty() ? ": heartbeat\n\n" : stream, "text/event-stream");
                return;
            }
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider("text/event-stream", [this, after](size_t offset, httplib::DataSink& sink) mutable {
                std::string output;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (offset > 0) wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stopping_ || event_sequence_ > after; });
                    if (stopping_) { sink.done(); return false; }
                    for (const auto& event : events_) {
                        const auto sequence = event.at("sequence").get<uint64_t>();
                        if (sequence <= after) continue;
                        output += "id: " + std::to_string(sequence) + "\nevent: " + event.at("type").get<std::string>() + "\ndata: " + event.dump() + "\n\n";
                        after = sequence;
                    }
                }
                if (output.empty()) output = ": heartbeat\n\n";
                return sink.write(output.data(), output.size());
            });
            return;
        }
        std::string path;
        if (operation == "passkey/challenge") {
            path = "/passkey/challenge";
            if (req.has_param("register") && req.get_param_value("register") == "1") path += "?register=1";
        } else if (operation == "passkey/register") path = "/passkey/register";
        else if (operation == "passkey/unlock") path = "/unlock";
        else if (operation == "invites") path = "/link-token";
        else if (operation == "invites/cancel") path = "/link-token/cancel";
        else if (operation == "devices/remove") path = "/devices/" + req.matches[1].str() + "/remove";
        else path = "/" + operation;
        auto result = control_locked(req.method, path, body);
        if (result.first == 200) {
            if (operation == "invites") active_invite_ = result.second;
            if (operation == "invites/cancel" || operation == "lock") active_invite_ = nullptr;
            if (result.second.is_object() && result.second.contains("mesh_up")) update_snapshot_locked(result.second);
        }
        reply(result.first, result.second);
    } catch (const std::invalid_argument&) {
        reply(400, {{"error", "Invalid request fields"}});
    } catch (const std::exception& e) {
        reply(500, {{"error", e.what()}});
    }
}

void NexusManager::register_routes(httplib::Server& server, bool management) {
    const auto dispatch = [this, management](const httplib::Request& req, httplib::Response& res, const std::string& operation) {
        if (management || operation == "status" || operation == "join/status") { handle(req, res, operation); return; }
        res.set_redirect("http://127.0.0.1:" + std::to_string(management_port_) + req.target, 307);
    };
    for (const auto* prefix : {"/api/v0/nexus/", "/api/v1/nexus/", "/v0/nexus/", "/v1/nexus/", "/api/nexus/"}) {
        for (const auto* operation : {"status", "passkey/challenge", "devices", "join/status", "events"}) {
            server.Get(std::string(prefix) + operation, [dispatch, operation](const auto& req, auto& res) { dispatch(req, res, operation); });
        }
        for (const auto* operation : {"enable", "disable", "passkey/register", "passkey/unlock", "invites", "invites/cancel", "join", "egress", "lock", "device"}) {
            server.Post(std::string(prefix) + operation, [dispatch, operation](const auto& req, auto& res) { dispatch(req, res, operation); });
        }
        server.Post(std::string(prefix) + R"(devices/([A-Za-z0-9_-]+)/remove)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "devices/remove"); });
    }
}
}
