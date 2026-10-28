#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace lnsdk {

class LemonadeNexusClient;
struct MeshPortProxies;

/// Default loopback port of the mesh port listener.
inline constexpr uint16_t kMeshPortDefaultPort = 13306;
/// Mesh virtual port where a Tier 3 device's lemond accepts mesh peers.
inline constexpr uint16_t kMeshPortPeerServicePort = 13305;

/// One device of this device's group, as the mesh port sees it.
struct MeshPortMember {
    std::string node_id;
    std::string device_name;    ///< Tree node hostname.
    std::string tunnel_ip;      ///< Mesh address, with or without a prefix length.
    bool        is_inference{false};  ///< Tier 3 inference node. False is Tier 4.
    bool        is_online{false};     ///< This device, or a listed mesh peer that is online.
    /// This device's key holds connect_private, connect_shared or admin on the node.
    bool        can_connect{false};
    double      latency_ms{0.0};      ///< Mesh latency; 0 when unknown.
};

/// What an inference device offers, as read from its /api/v1/health and /api/v1/models.
struct MeshPortPeerModels {
    std::string              node_id;
    std::string              device_name;
    double                   latency_ms{0.0};
    nlohmann::json           loaded = nlohmann::json::array();  ///< health all_models_loaded entries
    std::vector<std::string> available;                         ///< /models ids
    /// HTTP status of the /models probe; 0 when the device did not answer in time.
    /// 401 or 403 means the device refused the peer key, so it offers no models to
    /// this device until the key changes.
    int                      models_status{0};
};

/// Where the auto router sends one request. `target` is "local" or a node ID and
/// `model` is the model name to send. An empty target carries an HTTP error status
/// and message. `refused` names the devices that were skipped because they refused
/// the peer key (models_status 401 or 403).
struct MeshPortAutoRoute {
    std::string              target;
    std::string              model;
    int                      status{0};
    std::string              error;
    std::vector<std::string> refused;
};

/// A decrypted device certificate.
struct MeshPortCertificate {
    std::string domain;         ///< Certificate name, for example "*.team.lo.example".
    std::string fullchain_pem;
    std::string privkey_pem;
    uint64_t    expires_at{0};  ///< Unix seconds; 0 when unknown.
};

/// Routing rules of the mesh port. These functions are pure and thread-safe.
namespace mesh_port {

/// A device name as a DNS label: lowercase letters, digits and single hyphens, at
/// most 63 characters. The server's rule (LemonadeNexus/Tree/DeviceLabel.hpp):
/// the tree refuses a device whose name folds to a sibling's label, so a name
/// the tree accepted routes to one device here.
[[nodiscard]] std::string device_label(std::string_view device_name);

/// The local domain of a group wildcard certificate name. "*.<group>.lo.<base>"
/// returns ".<group>.lo.<base>". Every other name returns "", so the mesh port
/// does not serve TLS with it.
[[nodiscard]] std::string local_domain_from_certificate(std::string_view domain);

/// Maps a request to a device. `host` is the Host header and `sni` the TLS server
/// name (empty without TLS). A nonempty `sni` must name the same host as `host`,
/// otherwise the result is 421. Loopback names and this device's
/// "<label><local_domain>" return {0, "local"}; another member's
/// "<label><local_domain>" returns {0, its node ID}; an unknown label returns 404
/// and a name outside `local_domain` returns 421. A label that is a member's node id
/// always names that member. A label that several device names fold to ("Work PC" and
/// "work-pc") returns 409 and names the devices: the caller must use a node id.
/// `local_domain` starts with a dot.
[[nodiscard]] std::pair<int, std::string> host_target(
    std::string_view host, std::string_view sni, std::string_view local_domain,
    const std::vector<MeshPortMember>& members, std::string_view self_node_id);

/// Checks that the mesh port may forward to `node_id`. Returns {0, mesh address
/// without prefix length}, or an HTTP error: 404 for an unknown device, 409 for this
/// device, a device that is not Tier 3, offline or without a mesh address, and 403
/// when this device does not hold connect_private, connect_shared or admin on it.
[[nodiscard]] std::pair<int, std::string> forward_target(
    const std::vector<MeshPortMember>& members, std::string_view self_node_id,
    std::string_view node_id);

/// Picks the device for `model`. A name that matches an entry of `local_models`
/// exactly stays local, so a local model ID that contains '/' is never read as a
/// device name. Otherwise a "<device-name-or-node>/<model>" name goes to that device
/// with the prefix removed; otherwise an online Tier 3 peer that this device may
/// connect to and that has the model loaded wins (idle before busy, then lowest
/// latency), then such a peer that lists it. A peer whose models_status is 401 or
/// 403 refused the peer key: it is never chosen and is named in `refused`. An empty
/// model stays local.
[[nodiscard]] MeshPortAutoRoute auto_route(
    const std::string& model, const std::vector<MeshPortMember>& members,
    std::string_view self_node_id, const std::vector<std::string>& local_models,
    const std::vector<MeshPortPeerModels>& peers);

/// The local OpenAI model list plus "<device>/<model>" entries for each peer.
[[nodiscard]] nlohmann::json merge_models(const nlohmann::json& local,
                                          const std::vector<MeshPortPeerModels>& peers);

/// [{node_id, device_name, url, self}] for this device and each online Tier 3 member
/// that this device may connect to. Without a local domain, only this device's
/// "http://127.0.0.1:<port>" is listed. A device whose name folds to the same label
/// as another member's, or to no label at all, is listed under its node id, so every
/// URL routes to exactly one device.
[[nodiscard]] nlohmann::json device_urls(const std::vector<MeshPortMember>& members,
                                         std::string_view self_node_id,
                                         std::string_view local_domain, int port, bool tls);

/// Parses a local service URL. Only "http://127.0.0.1:<port>", "http://localhost:<port>"
/// and "http://[::1]:<port>" are accepted, with an optional trailing "/". Returns the
/// host to dial and the port, or nullopt.
[[nodiscard]] std::optional<std::pair<std::string, uint16_t>> parse_local_service_url(
    std::string_view url);

}  // namespace mesh_port

/// The operations the mesh port needs from the mesh session. Production code uses
/// make_mesh_port_backend(); tests supply fakes. Each function must be thread-safe.
struct MeshPortBackend {
    /// Reads the group devices. Called only on the mesh port's refresh thread, so it
    /// may make network calls. Returns nullopt when the list cannot be read.
    std::function<std::optional<std::vector<MeshPortMember>>()> load_members;
    /// This device's node ID, or "" before it joins.
    std::function<std::string()> self_node_id;
    /// Opens or reuses a loopback bridge to a mesh peer. Returns the 127.0.0.1 port,
    /// or 0 when the mesh cannot reach the peer.
    std::function<uint16_t(const std::string& tunnel_ip, uint16_t port)> open_egress;
    /// Closes the loopback bridge to a mesh peer. May be empty.
    std::function<void(const std::string& tunnel_ip, uint16_t port)> close_egress;
    /// Makes the group loopback certificate in `cert_dir` current and reads it.
    /// Production code calls LemonadeNexusClient::renew_certificate_if_needed() with
    /// kGroupLoopbackCertTarget, which also registers the certificate for background
    /// renewal; later renewals arrive through MeshPort::certificate_renewed(). Called
    /// only on the refresh thread, once per start or directory change. Returns nullopt
    /// on failure. May be empty: the port then stays on plain HTTP until a certificate
    /// arrives through certificate_renewed() or install_certificate().
    std::function<std::optional<MeshPortCertificate>(const std::string& cert_dir)> ensure_certificate;
};

/// Reads "fullchain.pem" and "privkey.pem" at the given paths. Returns nullopt when
/// a file cannot be read or is larger than 1 MiB.
[[nodiscard]] std::optional<MeshPortCertificate> read_certificate_files(
    const std::string& domain, const std::string& fullchain_path, const std::string& privkey_path);

/// Builds a backend over `client`. The client must outlive every MeshPort that uses
/// the backend.
[[nodiscard]] MeshPortBackend make_mesh_port_backend(LemonadeNexusClient& client);

/// Result of MeshPort::start().
enum class MeshPortStartResult {
    Ok,
    InvalidArgument,  ///< The local service URL is not a loopback HTTP URL.
    AlreadyRunning,   ///< The port is running on a different port number.
    PortInUse,        ///< Another socket owns 127.0.0.1:<port>. The port is not taken over.
    BindFailed,       ///< The listener could not bind for another reason.
};

/// Loopback listener that serves group devices by name.
///
/// The listener binds 127.0.0.1 only. It serves plain HTTP until the group loopback
/// certificate ("*.<group-label>.lo.<base>") is available, then TLS. The first
/// certificate replaces the plain listener with a TLS listener on the same port.
/// That switch waits until no request is open on the plain listener, so a running
/// stream is never cut; the status reports tls.state "activating" meanwhile and the
/// plain listener keeps serving. The SDK certificate renewer keeps the certificate
/// current; certificate_renewed() reloads it in place: the TLS listener and its
/// connections stay up and each new handshake uses the new certificate. A request
/// to another device's
/// "<device>.<group>.lo.<base>" name is forwarded over mesh egress to that device's
/// lemond on mesh port 13305. A request to this device's name or a loopback name is
/// forwarded to the current selection: the local service, the auto router or one
/// Tier 3 device. The listener refuses a request when no selection is set or the
/// selected device is offline; it never falls back to another device.
///
/// Exactly one process owns the port. start() binds without SO_REUSEPORT and returns
/// PortInUse when another socket already owns the port; it does not retry.
///
/// All public functions are thread-safe. The destructor stops the listener.
class MeshPort {
public:
    struct Options {
        /// Interval between member list refreshes.
        std::chrono::milliseconds refresh_interval{std::chrono::seconds(10)};
        /// How long cached model lists stay fresh for the auto router and the merged
        /// model listing. A peer is probed at most once per interval and per peer key.
        std::chrono::milliseconds models_ttl{std::chrono::seconds(5)};
        /// Connect timeout of one request to a peer's /api/v1/health or /api/v1/models.
        std::chrono::milliseconds peer_probe_connect_timeout{std::chrono::seconds(1)};
        /// Deadline of one round of peer probes. The peers are probed concurrently;
        /// a peer that has not answered by the deadline counts as offering no models
        /// until the next round.
        std::chrono::milliseconds peer_probe_deadline{std::chrono::seconds(3)};
    };

    explicit MeshPort(MeshPortBackend backend);
    MeshPort(MeshPortBackend backend, Options options);
    ~MeshPort();
    MeshPort(const MeshPort&) = delete;
    MeshPort& operator=(const MeshPort&) = delete;

    /// Binds 127.0.0.1:`port` (0 selects kMeshPortDefaultPort) and starts the listener
    /// and the refresh thread. `local_service_url` is the service that "local" serves,
    /// or "" for none. When the port already runs on the same port number, only the
    /// local service URL changes.
    MeshPortStartResult start(uint16_t port, const std::string& local_service_url);

    /// Stops the listener and the refresh thread, and clears the selection and the peer
    /// key. Does nothing when the port is not running. Blocks until open requests end
    /// or are aborted.
    void stop();

    /// Sets the selection target: "local", "auto", a node ID, or "" to clear it.
    /// `peer_key` is sent as "Bearer <key>" to the selected device's lemond. It is kept
    /// only in memory and replaced, or cleared, by every call. Returns 0 or an HTTP
    /// status: 409 when the port is not running, "local" has no local service, or the
    /// device is not Tier 3; 404 for an unknown device; 403 when this device may not
    /// connect to it. An offline Tier 3 device can be selected; requests then get 409.
    int set_selection(const std::string& target, const std::string& peer_key);

    /// Status JSON: {running, port, scheme, local_domain, local_service, error,
    /// selection: {target, peer_key_set}, tls: {state, domain, expires_at, error},
    /// urls: [{node_id, device_name, url, self}],
    /// devices: [{node_id, device_name, is_online}]}. tls.state is "off", "waiting",
    /// "activating" (the first certificate is installed; TLS starts when the open
    /// plain requests finish), "active", "unsupported_certificate" or "error".
    /// `devices` lists the Tier 3 devices that this device may connect to. The peer
    /// key is never included.
    [[nodiscard]] nlohmann::json status() const;

    /// Installs a device certificate. `domain` must be a group wildcard
    /// ("*.<group>.lo.<base>") that the certificate covers, and the key must match the
    /// certificate. The first certificate switches the listener to TLS: at once when
    /// no request is open, otherwise as soon as the last open plain request finishes
    /// (tls.state "activating" until then). A request that arrives during the few
    /// milliseconds of the switch itself gets 503 with Retry-After. Later
    /// certificates replace the one in use without touching the listener: open
    /// connections continue and new handshakes present the new certificate. Returns
    /// false and keeps the current state for every other input. Requires a running
    /// port.
    bool install_certificate(const MeshPortCertificate& certificate);

    /// Sets the directory of the group loopback certificate ("" for none). The
    /// refresh thread then provisions the certificate through
    /// MeshPortBackend::ensure_certificate. Without a directory the port stays on
    /// plain HTTP.
    void set_certificate_dir(const std::string& cert_dir);

    /// Reloads the certificate after a renewal of the group loopback target
    /// (kGroupLoopbackCertTarget). Events for other targets, or for another
    /// directory than the one set, are ignored. Safe to call from any thread.
    void certificate_renewed(const std::string& target, const std::string& domain,
                             const std::string& fullchain_path, const std::string& privkey_path);

    /// Runs one member refresh, and the certificate provisioning or the pending switch
    /// to TLS when one is due, now on the calling thread.
    void refresh_now();

private:
    struct Tls;
    struct Listener;

    void refresh_loop();
    void refresh_members();
    void check_certificate();
    void activate_pending_tls();
    bool activate_tls_locked();
    void handle(const httplib::Request& req, httplib::Response& res, bool tls);
    void serve_selection(const httplib::Request& req, httplib::Response& res,
                         const std::string& path);
    void serve_auto(const httplib::Request& req, httplib::Response& res,
                    const std::string& path, const std::string& peer_authorization);
    void forward_to_node(const std::string& node_id, const std::string& path,
                         const httplib::Request& req, httplib::Response& res,
                         const std::string& peer_authorization, const std::string& body);
    void forward_local(const std::string& path, const httplib::Request& req,
                       httplib::Response& res, const std::string& body);
    std::vector<std::string> local_model_ids(const std::string& authorization);
    nlohmann::json local_models(const std::string& authorization);
    std::vector<MeshPortPeerModels> peer_models(const std::vector<MeshPortMember>& members,
                                                const std::string& self,
                                                const std::string& peer_authorization);
    MeshPortStartResult bind_listener(uint16_t port, bool tls);
    void stop_listener();

    MeshPortBackend backend_;
    Options options_;

    // Serializes start() and stop(). Lock before lifecycle_mutex_.
    std::mutex control_mutex_;
    // Guards the listener and the HTTP to TLS switch. Lock before state_mutex_.
    std::mutex lifecycle_mutex_;
    std::unique_ptr<Listener> listener_;
    std::unique_ptr<Tls> tls_;
    std::shared_ptr<MeshPortProxies> proxies_;

    mutable std::mutex state_mutex_;
    bool running_{false};
    uint16_t port_{0};
    std::string local_host_;    // local service host; empty for none
    uint16_t local_port_{0};
    std::vector<MeshPortMember> members_;
    std::string local_domain_;
    std::string tls_state_{"waiting"};
    std::string tls_domain_;
    std::string tls_error_;
    uint64_t certificate_expires_at_{0};  // Not After of the installed certificate
    std::string cert_dir_;
    bool certificate_pending_{false};     // provision cert_dir_ on the next refresh
    bool tls_pending_{false};             // switch the plain listener to TLS once idle
    std::string error_;
    // The selection target ("", "local", "auto" or a node ID) and its peer key. The
    // key lives only in memory and is cleared whenever the target is set.
    std::string selection_target_;
    std::string selection_key_;
    std::map<std::string, std::string> egress_ips_;  // node ID -> mesh address

    std::mutex refresh_mutex_;  // serializes refresh_now()
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    bool refresh_requested_{false};
    std::thread refresh_thread_;

    struct CachedModels {
        std::chrono::steady_clock::time_point fetched;
        MeshPortPeerModels models;
    };
    std::mutex cache_mutex_;
    std::map<std::pair<std::string, std::string>, CachedModels> peer_cache_;  // (node, auth)
    std::string local_models_authorization_;
    std::chrono::steady_clock::time_point local_models_fetched_{};
    std::vector<std::string> local_models_;
};

}  // namespace lnsdk
