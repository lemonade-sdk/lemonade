#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <httplib.h>
#include <map>
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
/// `peer_authorization` ("Bearer <key>" or empty) becomes the upstream Authorization;
/// `body` replaces the request body unless the request is multipart. A POST whose JSON
/// body sets "stream": true is streamed like a backend response; other replies keep the
/// peer's status and body.
void relay_http(uint16_t port, const std::string& path, const httplib::Request& req, httplib::Response& res,
    const std::string& peer_authorization, const std::string& body);

/// What an inference device offers, as read from its /health and /models.
struct PeerModels {
    std::string node_id;
    std::string device_name;
    double latency_ms = 0;
    nlohmann::json loaded = nlohmann::json::array();     // /health all_models_loaded entries
    nlohmann::json available = nlohmann::json::array();  // /models ids
};
/// Where the auto router sends one request: `target` is "local" or a node ID, `model` is
/// the model name to send. An empty target carries an HTTP error status and message.
struct AutoRoute {
    std::string target;
    std::string model;
    int status = 0;
    std::string error;
};
/// Picks the device for `model`. A "<device-name-or-node>/<model>" name goes to that
/// device with the prefix removed; a name in `local_models` stays local; otherwise an
/// online Tier 3 peer with the model loaded wins (idle before busy, then lowest latency),
/// then one that lists it. Only online Tier 3 members other than this device count.
AutoRoute auto_route(const std::string& model, const nlohmann::json& members, const std::string& self_node_id,
    const std::vector<std::string>& local_models, const std::vector<PeerModels>& peers);
/// The local OpenAI model list plus "<device>/<model>" entries for each peer.
nlohmann::json merge_models(const nlohmann::json& local, const std::vector<PeerModels>& peers);
/// A device name as a DNS label: lowercase letters, digits and single hyphens, at most
/// 63 characters.
std::string device_label(const std::string& device_name);
/// Maps a request on the selection listener to a device. `host` is the Host header and
/// `sni` the TLS server name (empty without TLS); they must name the same host. Loopback
/// names and "<this device>.<local_domain>" return {0, "local"}; "<label>.<local_domain>"
/// of another member returns {0, its node ID}; anything else returns an HTTP error.
/// `local_domain` starts with a dot, for example ".team.lo.example".
std::pair<int, std::string> host_target(const std::string& host, const std::string& sni,
    const std::string& local_domain, const nlohmann::json& members, const std::string& self_node_id);
/// [{node_id, device_name, url}] for this device and each online Tier 3 member; see
/// docs/dev/nexus-integration.md.
nlohmann::json device_urls(const nlohmann::json& members, const std::string& self_node_id,
    const std::string& local_domain, int port, bool tls);
}

class NexusManager {
public:
    /// Starts the management and mesh ingress listeners on loopback ports.
    /// `install_policy` installs lemond's request middleware (origin, CORS, API keys) on the
    /// management listener. `install_api` installs the middleware and lemond's API routes on
    /// the mesh ingress listener; NexusManager registers its refusing Nexus routes first.
    /// Both run once, before the listener starts. If the mesh listener cannot bind, joining
    /// fails.
    /// Serves one request in this lemond, as its own routes would.
    using LocalHandler = std::function<void(const httplib::Request&, httplib::Response&)>;
    /// Group members and how to reach them. The default reads the mesh session.
    struct PeerDirectory {
        std::function<nlohmann::json()> members;
        std::function<std::string()> self_node_id;
        /// Opens a relay to `node_id` and sets its loopback port; see route_access for the
        /// result pair.
        std::function<std::pair<int, std::string>(const std::string& node_id, uint16_t& port)> open;
    };
    /// Also starts the selection listener on 127.0.0.1:`selection_port` (0 picks a free
    /// port). It serves lemond's API from the device chosen with POST nexus/selection and
    /// gets `install_policy`; `local` serves requests that select this device.
    NexusManager(const std::string& config_dir,
                 const std::function<void(httplib::Server&)>& install_policy,
                 const std::function<void(httplib::Server&)>& install_api,
                 LocalHandler local, int selection_port);
    ~NexusManager();
    void register_routes(httplib::Server& server, NexusListener listener = NexusListener::Public);
    void stop();
    nlohmann::json status() const;
    /// The port the selection listener is bound to; 0 when it could not bind.
    int selection_port() const { return selection_port_; }
    /// Replaces the mesh-backed peer directory, for tests that have no mesh.
    void set_peer_directory(PeerDirectory directory);
    /// `domain` is the certificate name, for example "*.team.lo.example". The first call
    /// switches port 13306 to TLS; later calls replace the certificate in place. Returns
    /// false for a rejected PEM or a build without server TLS.
    bool install_certificate(const std::string& domain, const std::string& fullchain_pem,
                             const std::string& private_key_pem);
    /// Sets the local domain without a certificate, for tests that run without TLS.
    void set_local_domain(const std::string& local_domain);
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
    void serve_selected(const httplib::Request& req, httplib::Response& res);
    void serve_own(const httplib::Request& req, httplib::Response& res, const std::string& path);
    void configure_selection_server(httplib::Server& server);
    void start_selection_server(int port);
    void renew_certificate();
    void serve_auto(const httplib::Request& req, httplib::Response& res, const std::string& path,
                    const std::string& peer_authorization);
    void forward_to(const std::string& node_id, const std::string& path, const httplib::Request& req,
                    httplib::Response& res, const std::string& peer_authorization, const std::string& body);
    json selection_reply() const;
    std::pair<int, std::string> select(const json& body);
    std::vector<std::string> local_model_ids();
    std::vector<nexus_access::PeerModels> peer_models(const json& members, const std::string& self,
                                                      const std::string& peer_authorization);
    PeerDirectory directory();
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
    LocalHandler local_;
    std::function<void(httplib::Server&)> install_policy_;
    // Plain HTTP until the first device certificate arrives, then httplib::SSLServer.
    std::unique_ptr<httplib::Server> selection_server_;
    std::atomic<bool> selection_tls_{false};
    std::string local_domain_;  // guarded by mutex_
    int64_t certificate_expires_at_{0};  // epoch seconds, 0 when unknown; guarded by session_mutex_
    std::chrono::steady_clock::time_point certificate_attempt_{};
    std::thread selection_thread_;
    int selection_port_{0};
    // Machine-wide inference target for the selection listener ("", "local", "auto" or a
    // node ID) and the peer key used with it. Both are guarded by mutex_; the key lives
    // only in memory and is cleared whenever the target changes.
    std::string selection_target_;
    std::string selection_key_;
    PeerDirectory test_directory_;
    struct CachedPeer {
        std::chrono::steady_clock::time_point fetched;
        nexus_access::PeerModels models;
    };
    std::mutex peer_cache_mutex_;
    std::map<std::string, CachedPeer> peer_cache_;
    std::chrono::steady_clock::time_point local_models_fetched_{};
    std::vector<std::string> local_models_;
};
}
