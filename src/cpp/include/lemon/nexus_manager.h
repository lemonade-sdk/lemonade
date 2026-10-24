#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lemon {
/// The listener a Nexus route is registered on.
/// Public: lemond's normal port. Loopback clients read status and are redirected to
/// Management for every other route. Nonloopback clients may call the group operations
/// accepted by nexus_access::remote_operation, only with LEMONADE_API_KEY configured.
/// Management: the loopback listener owned by NexusManager for the local desktop.
/// MeshIngress: the loopback listener behind Nexus virtual port 13305. Mesh peers arrive
/// from loopback, so every Nexus route on it is refused.
enum class NexusListener { Public, Management, MeshIngress };

namespace nexus_access {
extern const std::vector<std::string> kPermissions;
bool same_key(const std::string& a, const std::string& b);
bool grants(const nlohmann::json& permissions, const std::string& permission);
nlohmann::json account_summary(const nlohmann::json& members, const std::string& group_id,
    const std::string& node_id, const std::string& identity_id);
/// Validate bounded account avatar metadata against this session's group.
nlohmann::json account_avatar(const nlohmann::json& value, const std::string& group_id);
nlohmann::json tree_path(const std::string& id, const std::string& root_id,
    const std::function<nlohmann::json(const std::string&)>& fetch);
nlohmann::json tree_entry(const nlohmann::json& node, const std::string& self_key,
    bool ancestor_admin = false);
nlohmann::json merge_members(const nlohmann::json& group, const nlohmann::json& children,
    const nlohmann::json& peers, const std::string& self_key, const std::string& self_node_id);
nlohmann::json device_access(const nlohmann::json& members, const std::string& device_id,
    const std::string& self_node_id);
nlohmann::json role_permissions(const nlohmann::json& current, bool admin);
nlohmann::json group_permissions(const nlohmann::json& requested, bool admin);
nlohmann::json device_permissions(const nlohmann::json& current, bool see, bool connect);
std::pair<int, std::string> assignment_error(const std::string& reason);
/// Compares two secrets in time that depends only on the length of `expected`.
bool constant_time_equals(const std::string& candidate, const std::string& expected);
/// True when `authorization` is "Bearer <key>" for `api_key` or a nonempty `admin_key`.
bool bearer_matches(const std::string& authorization, const std::string& api_key, const std::string& admin_key);
/// True for the operations a nonloopback client may call on the public listener:
/// the group operations and "relay".
bool remote_operation(const std::string& operation);
/// Decides whether a Nexus route may run. Origin, CORS and the API key are checked earlier
/// by lemond's shared listener policy; this adds the per-listener rules. Returns {0, ""}
/// to run the route, {307, ""} to redirect it to the management listener, or an HTTP
/// error status and message. An empty `api_key` turns remote management off.
std::pair<int, std::string> route_access(NexusListener listener, bool loopback, const std::string& operation,
    const std::string& authorization, const std::string& api_key, const std::string& admin_key);
/// Header that carries the selected peer's API key through the relay. The relay sends it
/// to that peer as Authorization and never forwards the caller's own Authorization.
extern const char* const kPeerAuthorizationHeader;
/// Chooses the relay target among merged group members. Returns {0, tunnel address without
/// prefix length} or an HTTP error status and message.
std::pair<int, std::string> relay_target(const nlohmann::json& members, const std::string& self_node_id,
    const std::string& node_id);
/// Sends a GET or POST `req` to http://127.0.0.1:`port``path` with lemond's HTTP client.
/// A POST whose JSON body sets "stream": true is streamed like a backend response;
/// other replies keep the peer's status and body.
void relay_http(uint16_t port, const std::string& path, const httplib::Request& req, httplib::Response& res);
}

class NexusManager {
public:
    /// Starts the management and mesh ingress listeners on loopback ports.
    /// `install_policy` installs lemond's request middleware (origin, CORS, API keys) on the
    /// management listener. `install_api` installs the middleware and lemond's API routes on
    /// the mesh ingress listener; NexusManager registers its refusing Nexus routes first.
    /// Both run once, before the listener starts. If the mesh listener cannot bind, joining
    /// fails.
    NexusManager(const std::string& config_dir,
                 const std::function<void(httplib::Server&)>& install_policy,
                 const std::function<void(httplib::Server&)>& install_api);
    ~NexusManager();
    void register_routes(httplib::Server& server, NexusListener listener = NexusListener::Public);
    void stop();
    nlohmann::json status() const;
    bool enable(std::string& error);
    void disable();
private:
    using json = nlohmann::json;
    struct Session;
    void supervise();
    void persist_enabled_locked();
    void update_snapshot_locked(const json& snapshot);
    void emit_locked(const std::string& type, const json& data);
    void handle(const httplib::Request& req, httplib::Response& res, const std::string& operation, bool remote);
    void relay(const httplib::Request& req, httplib::Response& res);
    std::pair<int, json> control(const std::string& method, const std::string& path, const json& body);
    std::string data_root_;
    int ingress_port_{0};
    std::unique_ptr<Session> session_;
    mutable std::mutex mutex_;
    // Serializes Session (SDK) work. Lock before mutex_ when both are needed.
    std::mutex session_mutex_;
    std::condition_variable wake_;
    bool enabled_{false};
    bool stopping_{false};
    json snapshot_ = {{"status", "disabled"}, {"locked", true}, {"mesh_up", false}, {"members", json::array()}};
    json active_invite_ = nullptr;
    uint64_t event_sequence_{0};
    std::deque<json> events_;
    std::thread worker_;
    httplib::Server management_server_;
    std::thread management_thread_;
    int management_port_{0};
    httplib::Server mesh_server_;
    std::thread mesh_thread_;
};
}
