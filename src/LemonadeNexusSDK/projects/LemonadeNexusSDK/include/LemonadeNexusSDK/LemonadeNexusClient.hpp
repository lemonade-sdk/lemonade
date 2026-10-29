#pragma once

#include <LemonadeNexusSDK/CertRenewal.hpp>
#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Identity.hpp>
#include <LemonadeNexusSDK/LatencyMonitor.hpp>
#include <LemonadeNexusSDK/RoutingTypes.hpp>
#include <LemonadeNexusSDK/ServiceIngress.hpp>
#include <LemonadeNexusSDK/Tier1Directory.hpp>
#include <LemonadeNexusSDK/Types.hpp>
#include <LemonadeNexusSDK/BoringtunMesh.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lnsdk {

/// Why `invite` is neither an invite code nor a link token, or nullopt when
/// it is one of the two. An invite code is exactly 8 decimal digits; a link
/// token is "lnk_" followed by 64 hex digits. Both name the same single-use
/// invite and are accepted wherever a link token is. The SDK runs this check
/// before it sends anything (set_link_token, accept_invite,
/// check_device_name, and the C ABI), so a mistyped invite costs no request.
[[nodiscard]] std::optional<std::string> invite_format_error(std::string_view invite);

/// Standalone C++ client for the Lemonade-Nexus server.
///
/// Uses PIMPL to hide httplib and internal state from the public API.
/// Thread-safe for concurrent calls from multiple threads.
class LemonadeNexusClient {
public:
    explicit LemonadeNexusClient(const ServerConfig& config);
    ~LemonadeNexusClient();

    // Non-copyable, movable
    LemonadeNexusClient(const LemonadeNexusClient&) = delete;
    LemonadeNexusClient& operator=(const LemonadeNexusClient&) = delete;
    LemonadeNexusClient(LemonadeNexusClient&&) noexcept;
    LemonadeNexusClient& operator=(LemonadeNexusClient&&) noexcept;

    // -----------------------------------------------------------------
    // Identity management
    // -----------------------------------------------------------------

    /// Set the client identity (Ed25519 keypair) used for delta signing.
    void set_identity(const Identity& identity);

    /// Get the current identity (may be invalid if not set).
    /// Returns by value to avoid data races with concurrent set_identity().
    [[nodiscard]] Identity identity() const;

    // -----------------------------------------------------------------
    // Tier 1 servers (tree traffic)
    // -----------------------------------------------------------------

    /// Pin the network's Genesis public key (base64 Ed25519, the server
    /// config's `genesis_pubkey`) and the lowest epoch to accept. Every tree
    /// read and write, every session request (/api/auth...), and every
    /// private-API call goes only to a Tier 1 server the client verified from
    /// this key (see Tier1Directory). Without it the SDK sends none of them
    /// and those calls fail with an error that names the missing key.
    /// `min_epoch` lets an application that stored the last verified epoch
    /// refuse an older membership after a restart. Errors: a malformed key.
    [[nodiscard]] StatusResult set_genesis_key(const std::string& genesis_pubkey_b64,
                                               uint64_t min_epoch = 0);

    /// First-connect discovery of the Genesis key, for a client with no
    /// pinned key. Asks `hosts` (for example, servers the application found
    /// through DNS discovery), then the configured servers and the servers
    /// they list in /api/servers (when `auto_discover` is set), for
    /// GET /api/onboard/info over verified HTTPS. On success the key is
    /// pinned as with set_genesis_key(); store the returned key and pass it
    /// to set_genesis_key() on later runs. It never replaces a pinned key.
    /// See Tier1Directory::discover_genesis_key for the agreement rules and
    /// the trust limit (trust on first use). Errors: a key is already pinned,
    /// the servers disagree or send a malformed key, or too few servers
    /// agree. Blocks on network I/O.
    [[nodiscard]] Result<GenesisKeyDiscovery> discover_genesis_key(
        const std::vector<ServerEndpoint>& hosts = {});

    /// Verify the Tier 1 membership now, from the current server pool. Tree
    /// calls also refresh it on their own when it is older than five minutes
    /// or a server refused a tree route. Errors: no pinned key, a chain that
    /// does not verify, a stale epoch, or no verified server reachable.
    [[nodiscard]] Result<Tier1Membership> refresh_tier1();

    /// The last verified membership (empty when none is verified).
    [[nodiscard]] Tier1Membership tier1_membership() const;

    // -----------------------------------------------------------------
    // Health
    // -----------------------------------------------------------------

    /// GET /api/health. With a session, it also fails once when the session's
    /// server is no longer a verified Tier 1 server. The SDK then moves the
    /// session by itself when it holds an identity (see
    /// set_session_moved_callback); otherwise it drops the session (see
    /// MeshTunnelStatus::needs_rejoin) so the caller joins again.
    [[nodiscard]] Result<HealthStatus> check_health();

    // -----------------------------------------------------------------
    // Authentication
    // -----------------------------------------------------------------

    /// POST /api/auth — Ed25519 identity authentication (primary method).
    /// Performs two-phase challenge-response using the client's Ed25519 keypair.
    /// Requires a valid identity (call set_identity() first).
    /// Auto-registers new pubkeys on first use.
    [[nodiscard]] Result<AuthResponse> authenticate_ed25519();

    /// POST /api/auth — password authentication (deprecated, stub on server).
    [[nodiscard]] Result<AuthResponse> authenticate(const std::string& username,
                                                     const std::string& password);

    /// POST /api/auth — passkey/FIDO2 authentication (backup method).
    [[nodiscard]] Result<AuthResponse> authenticate_passkey(const nlohmann::json& passkey_data);

    /// POST /api/auth/challenge — issue the WebAuthn assertion challenge bound
    /// to `user_id`. The assertion's clientDataJSON must carry the returned
    /// challenge; it expires server-side and is single-use.
    [[nodiscard]] Result<std::string> issue_passkey_challenge(const std::string& user_id);

    /// POST /api/auth — token-link authentication.
    [[nodiscard]] Result<AuthResponse> authenticate_token(const std::string& token);

    /// POST /api/auth — passkey registration flow (legacy).
    [[nodiscard]] Result<AuthResponse> register_passkey(const PasskeyRegistration& reg);

    /// POST /api/auth/register — register a passkey credential.
    [[nodiscard]] Result<AuthResponse> register_passkey_credential(const std::string& user_id,
                                                                     const PasskeyCredential& cred);

    // -----------------------------------------------------------------
    // Tree operations
    // -----------------------------------------------------------------

    /// GET /api/tree/node/{id}
    [[nodiscard]] Result<TreeNode> get_tree_node(const std::string& node_id);

    /// POST /api/tree/delta — submit a raw (signed) delta.
    [[nodiscard]] Result<DeltaResult> submit_delta(const TreeDelta& delta);

    /// Convenience: create_node delta under parent_id.
    [[nodiscard]] Result<DeltaResult> create_child_node(const std::string& parent_id,
                                                         const TreeNode& child);

    /// Convenience: update_node delta.
    [[nodiscard]] Result<DeltaResult> update_node(const std::string& node_id,
                                                    const nlohmann::json& updates);

    /// Convenience: delete_node delta.
    [[nodiscard]] Result<DeltaResult> delete_node(const std::string& node_id);

    /// GET /api/tree/children/{parent_id}
    [[nodiscard]] Result<std::vector<TreeNode>> get_children(const std::string& parent_id);

    // -----------------------------------------------------------------
    // IPAM
    // -----------------------------------------------------------------

    /// POST /api/ipam/allocate
    [[nodiscard]] Result<AllocationResponse> allocate_ip(const AllocationRequest& req);

    /// Convenience: allocate a tunnel IP for node_id.
    [[nodiscard]] Result<AllocationResponse> allocate_tunnel_ip(const std::string& node_id);

    // -----------------------------------------------------------------
    // Relay
    // -----------------------------------------------------------------

    /// GET /api/relay/list
    [[nodiscard]] Result<std::vector<RelayNodeInfo>> list_relays();

    /// POST /api/relay/ticket
    [[nodiscard]] Result<RelayTicket> request_relay_ticket(const std::string& peer_id,
                                                            const std::string& relay_id);

    /// POST /api/relay/register
    [[nodiscard]] Result<RelayRegisterResult> register_relay(const RelayRegistration& reg);

    // -----------------------------------------------------------------
    // Certificates
    // -----------------------------------------------------------------

    /// GET /api/certs/{domain} — check if a certificate exists.
    [[nodiscard]] Result<CertStatus> get_cert_status(const std::string& domain);

    /// POST /api/certs/issue — request a TLS certificate for this client.
    /// Server obtains the cert via ACME and returns it encrypted with our key.
    /// @param hostname The client hostname (e.g. "my-laptop")
    /// @return Encrypted certificate bundle (call decrypt_certificate to use)
    [[nodiscard]] Result<IssuedCertBundle> request_certificate(const std::string& hostname);

    /// Decrypt an issued certificate bundle using our Ed25519 identity.
    /// Performs X25519 DH with the server's ephemeral pubkey, derives the AEAD
    /// key via HKDF, and decrypts the private key.
    /// @param bundle The encrypted bundle from request_certificate()
    /// @return Decrypted certificate (fullchain PEM + private key PEM)
    [[nodiscard]] Result<DecryptedCert> decrypt_certificate(const IssuedCertBundle& bundle);

    /// POST /api/certs/issue with scope "group_loopback" — request the
    /// wildcard certificate "*.<group-label>.lo.<base_domain>" of this
    /// device's group. Every device of the group receives the same key. The
    /// server requires a session of a joined endpoint of the group.
    /// @return Encrypted certificate bundle (call decrypt_certificate to use)
    [[nodiscard]] Result<IssuedCertBundle> request_group_loopback_certificate();

    /// Callback type for certificate renewals.
    using CertRenewedCallback = CertRenewer::RenewedCallback;

    /// Set the callback that runs once after each certificate renewal, when
    /// both files are replaced (empty clears it). It runs on the thread that
    /// renewed: the mesh orchestrator's renewal thread, or the caller of
    /// renew_certificate_if_needed(). When this returns, the previous callback
    /// is not running and is not called again. Do not call this method from
    /// the callback.
    void set_cert_renewed_callback(CertRenewedCallback cb);

    /// Keep the certificate of `target` in `cert_dir` current: request and
    /// decrypt a new one when the stored one is missing or less than one third
    /// of its lifetime remains, then replace `fullchain.pem` and `privkey.pem`
    /// (mode 0600) through temporary files. `target` is a node hostname or
    /// kGroupLoopbackCertTarget. The target is then also renewed in the
    /// background while the mesh is enabled. Blocks on the network when due.
    CertRenewalResult renew_certificate_if_needed(const std::string& target,
                                                  const std::string& cert_dir);

    /// True when a certificate registered with renew_certificate_if_needed()
    /// is due and outside its backoff delay. Reads only local files.
    [[nodiscard]] bool certificate_renewal_pending() const;

    /// Renew the registered certificates that are due and outside their
    /// backoff delay. Used by MeshOrchestrator; blocks on the network.
    void renew_managed_certificates();

    // -----------------------------------------------------------------
    // High-level composite operations
    // -----------------------------------------------------------------

    /// Authenticate → create endpoint node → allocate tunnel IP.
    ///
    /// With an invite (set_link_token), a verified Tier 1 server that answers
    /// 403 "invalid or expired link token" may only lack the mint, which
    /// replicates over the backbone. The join then waits (see
    /// set_invite_hop_delay) and joins through another verified server that
    /// has not refused it, up to three attempts in all, each server at most
    /// once. Each refusal counts against the server's wrong-code limit for
    /// this device's address. When no other server remains, the last refusal
    /// is returned. Blocks on network I/O and on these waits.
    [[nodiscard]] Result<JoinResult> join_network(const std::string& username,
                                                    const std::string& password);

    /// Submit delete_node delta for the current node.
    [[nodiscard]] StatusResult leave_network();

    // -----------------------------------------------------------------
    // Group membership management
    // -----------------------------------------------------------------

    /// Set exactly the permissions `pubkey` holds on `node_id`, replacing its
    /// entry; an empty list removes it. The caller must be an admin of the node
    /// or an ancestor, and `pubkey` must have joined inside that admin's group.
    /// The server refuses an admin's change to its own entry on the node that
    /// grants its admin. Errors: 403 not allowed, 400 unknown permission, 404
    /// no such node.
    [[nodiscard]] Result<DeltaResult> set_member_permissions(
            const std::string& node_id, const std::string& pubkey,
            const std::vector<std::string>& permissions);

    /// Give a member the listed permissions on a node (replaces its entry).
    /// Same rules as set_member_permissions.
    [[nodiscard]] Result<DeltaResult> add_group_member(const std::string& node_id,
                                                         const GroupMember& member);

    /// Remove a member's entry from a node. Same rules as set_member_permissions.
    [[nodiscard]] Result<DeltaResult> remove_group_member(const std::string& node_id,
                                                            const std::string& pubkey);

    /// Get the members (assignments) of a group node.
    [[nodiscard]] Result<std::vector<GroupMember>> get_group_members(const std::string& node_id);

    /// Join an existing group: create endpoint child under parent_node_id + allocate tunnel IP.
    [[nodiscard]] Result<GroupJoinResult> join_group(const std::string& parent_node_id);

    // -----------------------------------------------------------------
    // Routing layer (connect-by-identifier control plane)
    // -----------------------------------------------------------------

    /// GET /api/routing/profile — identity + authorized endpoint identifiers.
    [[nodiscard]] Result<RoutingProfile> get_routing_profile(int page = 0,
                                                             int page_size = 200);

    /// POST /api/routing/request — request a connection to an endpoint by id.
    /// conn_nonce_b64 is a client-chosen 16-byte nonce (base64).
    [[nodiscard]] Result<ConnectionRequestResult> request_endpoint(
        const std::string& identifier, const std::string& conn_nonce_b64,
        const std::string& client_mesh_pubkey = "",
        const std::vector<std::string>& candidates = {});

    /// POST /api/routing/connect — fetch the directive once the endpoint is ready.
    [[nodiscard]] Result<ConnectionDirective> routing_connect(
        const std::string& connection_id);

    /// GET /api/routing/session/{id} — current connection state.
    [[nodiscard]] Result<ConnectionStatus> connection_status(
        const std::string& connection_id);

    // Endpoint-role calls (for a node that exposes an inference endpoint).

    /// POST /api/routing/endpoint/register — returns any pending connection ids.
    [[nodiscard]] Result<nlohmann::json> routing_register_endpoint(
        const std::string& cpu_id, const std::string& net_mac,
        const std::string& mesh_pubkey, const std::string& stun_endpoint = "");

    /// POST /api/routing/endpoint/ready — signal readiness for a connection.
    [[nodiscard]] Result<nlohmann::json> routing_endpoint_ready(
        const std::string& connection_id, const std::string& cpu_id,
        const std::string& net_mac, const std::string& endpoint_mesh_pubkey = "",
        const std::vector<std::string>& candidates = {});

    // -----------------------------------------------------------------
    // Server discovery & health
    // -----------------------------------------------------------------

    /// Fetch /api/servers to discover additional servers in the pool.
    void discover_servers();

    /// Check health of all servers in the pool and update their status.
    void refresh_health();

    // -----------------------------------------------------------------
    // Stats & server listing
    // -----------------------------------------------------------------

    /// GET /api/stats
    [[nodiscard]] Result<StatsResponse> get_stats();

    /// GET /api/servers
    [[nodiscard]] Result<std::vector<ServerEntry>> get_servers();

    // -----------------------------------------------------------------
    // Private-API transport
    // -----------------------------------------------------------------

    /// Generic authenticated call to a private-API route over the mesh. `method`
    /// is "GET" or "POST"; `body` is JSON (ignored for GET). Returns the raw
    /// response body as a JSON string. The session JWT and TLS are applied by
    /// the private transport, so any authenticated private route is reachable.
    [[nodiscard]] Result<std::string> call_private_api(const std::string& method,
                                                       const std::string& path,
                                                       const std::string& body);

    // -----------------------------------------------------------------
    // DDNS status
    // -----------------------------------------------------------------

    /// GET /api/ddns/status
    [[nodiscard]] Result<DdnsStatus> get_ddns_status();

    // -----------------------------------------------------------------
    // Attestation manifests
    // -----------------------------------------------------------------

    /// GET /api/attestation/manifests
    [[nodiscard]] Result<AttestationManifests> get_attestation_manifests();

    // -----------------------------------------------------------------
    // Latency-based auto-switching
    // -----------------------------------------------------------------

    /// Enable automatic server switching based on latency monitoring.
    void enable_auto_switching(const LatencyConfig& config = {});

    /// Disable automatic server switching and stop the background monitor.
    void disable_auto_switching();

    /// Get the smoothed RTT (ms) to the current server. Returns 0.0 if monitoring is off.
    [[nodiscard]] double current_latency_ms() const;

    /// Get latency stats for all monitored servers.
    [[nodiscard]] std::vector<ServerLatency> server_latencies() const;

    // -----------------------------------------------------------------
    // Mesh P2P networking
    // -----------------------------------------------------------------

    /// Whether the userspace mesh dataplane is up (started at join).
    [[nodiscard]] bool is_mesh_active() const;

    /// Enable mesh networking. Starts background peer discovery, NAT traversal,
    /// and dataplane peer sync. Requires a joined node (node_id + dataplane).
    void enable_mesh(const MeshConfig& config = {});

    /// Disable mesh networking and remove all mesh peers from the tunnel.
    /// Waits for the orchestrator thread to stop, without holding the client
    /// lock, so a mesh state callback may call other client methods. Do not
    /// call it, enable_mesh(), or leave_network() from a mesh state callback.
    void disable_mesh();

    /// Get mesh tunnel status including all peers with live stats. Its
    /// `needs_rejoin` is true after the SDK dropped a session whose Tier 1
    /// server left the verified membership and could not move it by itself
    /// (no identity, or the silent join failed): call join_network() again,
    /// which authenticates through a current member and clears it.
    [[nodiscard]] MeshTunnelStatus mesh_status() const;

    /// Get the current list of known mesh peers.
    [[nodiscard]] std::vector<MeshPeer> get_mesh_peers() const;

    /// Force an immediate peer refresh (fetch from server + sync tunnel).
    void refresh_mesh_peers();

    /// Callback type for mesh state changes.
    using MeshStateCallback = std::function<void(const MeshTunnelStatus&)>;

    /// Set a callback to be notified on mesh state changes.
    void set_mesh_callback(MeshStateCallback cb);

    /// Callback type for session moves.
    using SessionMovedCallback = std::function<void(const SessionMoveEvent&)>;

    /// Set the callback that runs once after each session move (empty
    /// clears it). The session's Tier 1 server leaves the membership at an
    /// epoch boundary: it says so in its heartbeat answers before the
    /// handoff, and it refuses tree routes after it. On either sign, and
    /// when a membership refresh finds the server gone, the SDK verifies the
    /// Tier 1 membership again from the pinned Genesis key and, when the
    /// server is no longer a verified member (or leaves at a later epoch and
    /// another verified server exists), authenticates with the device
    /// identity through another verified Tier 1 server, joins again, and
    /// restarts the mesh orchestrator as it was. The notice never chooses the
    /// server: the SDK picks it from the membership it verified. The SDK also
    /// moves a session whose server stays a verified member but stops
    /// answering private requests over the mesh for 30 s (a server that
    /// restarted in place forgot this client's dataplane peer) while its
    /// public API still answers: it joins again through a verified member,
    /// the same one included, which adds the peer back. Without an
    /// identity, or when that join fails, the session is dropped and
    /// `needs_rejoin` is set (`silent` false). Runs on an SDK background
    /// thread, or on the thread whose call found the server gone, outside
    /// the client's locks. When this returns, the previous callback is not
    /// running and is not called again. Do not call this method, or destroy
    /// the client, from the callback.
    void set_session_moved_callback(SessionMovedCallback cb);

    /// Fetch mesh peers from the server for a given node_id.
    /// Used internally by MeshOrchestrator; also available for direct use.
    [[nodiscard]] Result<std::vector<MeshPeer>> fetch_mesh_peers(const std::string& node_id);

    /// Send a heartbeat to the server reporting our current endpoint.
    [[nodiscard]] StatusResult mesh_heartbeat(const std::string& node_id,
                                               const std::string& endpoint);

    // -----------------------------------------------------------------
    // Session state
    // -----------------------------------------------------------------

    /// Set the session token obtained from authentication. A session belongs
    /// to the Tier 1 server that issued it, and a token set here names no
    /// server: private-API calls stay refused until authenticate_*() or
    /// join_network() binds a session to a verified Tier 1 server.
    void set_session_token(const std::string& token);

    /// Get the current session token.
    /// Returns by value to avoid data races with concurrent set_session_token().
    [[nodiscard]] std::string session_token() const;

    /// Get the assigned node ID (set after join_network or set manually).
    /// Returns by value to avoid data races with concurrent set_node_id().
    [[nodiscard]] std::string node_id() const;

    /// Set the node ID manually.
    void set_node_id(const std::string& id);

    /// Set the single-use invite to include in the next join_network call
    /// (obtained out-of-band from an already-joined device): the 8-digit
    /// invite code or the full link token (invite_format_error). An empty
    /// string clears it. Returns false, and stores nothing, for any other
    /// value.
    bool set_link_token(const std::string& token);

    /// Join as an inference node (Tier 3) instead of a client (Tier 4). Sent
    /// with the next join_network; the server records it when it creates the
    /// endpoint and derives the routing identifier from it ("infer-..." vs
    /// "client-..."). Default false.
    void set_inference_node(bool inference);

    /// Set the wait before join_network() asks another Tier 1 server after
    /// one refused the invite as unknown. The second such hop waits twice as
    /// long. The default, 1.5 s, gives a fresh mint time to replicate; keep
    /// it outside tests. A negative value counts as zero. Thread-safe; takes
    /// effect at the next join_network().
    void set_invite_hop_delay(std::chrono::milliseconds first_hop);

    /// Mint a single-use link token (an invite) for a target group (requires
    /// an established mesh session; calls the private API). An empty
    /// `target_node_id` targets this account's own group. An empty
    /// `permissions` list lets the server choose the default ("read",
    /// "connect_private", reduced to what the caller holds). A new device
    /// joins under the target with the token (set_link_token + join_network);
    /// an account that already joined becomes a member (accept_invite). The
    /// server refuses a target where the caller lacks add_child, and any
    /// permission the caller does not hold or an invite cannot grant.
    /// Response JSON: {link_token, code, group_node_id, target_node_id,
    /// permissions, expires_at, code_expires_at}. `code` is the 8-digit
    /// invite code that names the same invite; it works for at most 10
    /// minutes (code_expires_at), the token until expires_at. Both are
    /// returned once and never again.
    [[nodiscard]] Result<nlohmann::json> create_link_token(
            uint32_t ttl_sec = 600, const std::string& target_node_id = {},
            const std::vector<std::string>& permissions = {});

    /// Accept an invite as an account that already joined: this identity
    /// becomes a member of the invite's target group with the invite's
    /// permissions. Nothing this account owns moves. The invite is single
    /// use. `link_token` is the 8-digit invite code or the full link token;
    /// any other value fails before a request is sent. Response JSON:
    /// {success, node_id, permissions, status: "member" | "pending", op_id?}.
    /// A refusal carries the server's "error".
    [[nodiscard]] Result<nlohmann::json> accept_invite(const std::string& link_token);

    /// The groups this identity belongs to. Response JSON:
    /// {memberships: [{node_id, hostname, parent_id, owner_pubkey,
    /// role: "home" | "member", permissions}]}.
    [[nodiscard]] Result<nlohmann::json> list_memberships();

    /// Leave a group this identity entered through an invite: removes this
    /// identity's own entry on `group_node_id`. The server refuses it for a
    /// home group (a group this identity joined or administers).
    [[nodiscard]] Result<DeltaResult> leave_membership(const std::string& group_node_id);

    /// Move a device or a subgroup under another group. Needs delete_node on
    /// the device (or admin above it), admin above a subgroup, and add_child
    /// on the new parent. A top-level group never moves. The node keeps its
    /// id, tunnel address, and own assignments. Response JSON: {success,
    /// node_id, parent_id}, or the server's "error".
    [[nodiscard]] Result<nlohmann::json> move_node(const std::string& node_id,
                                                   const std::string& new_parent_id);

    /// Ask a Tier 1 server whether a device could take `name` in a group now
    /// (POST /api/tree/device-name/check). Device names fold to one mesh-port
    /// label ("Work PC" and "work-pc" are both "work-pc"), and the tree
    /// refuses a join or a rename whose label another device of the same
    /// group holds, with HTTP 409. Give `group_node_id` (needs read on the
    /// group) or `link_token` (an invite, as its 8-digit code or full token:
    /// its target group is checked and the invite is not spent); one of the
    /// two, not both empty. Advisory: a name
    /// reported available can still be taken before it is used. Needs a
    /// session; works before a join, so a new device can check the name it
    /// will use with an invite.
    [[nodiscard]] Result<DeviceNameCheck> check_device_name(const std::string& name,
                                                            const std::string& group_node_id,
                                                            const std::string& link_token);

    /// Permanently delete this identity's account. Irreversible. Requests a
    /// challenge from the session's server (POST /api/account/delete/challenge),
    /// signs the account deletion proof with this identity's key, and sends
    /// it (POST /api/account/delete). The server removes the account's groups,
    /// devices, and everything beneath them (other users' devices placed
    /// there lose access to this network), this identity's memberships and
    /// grants, its key envelopes, chat blobs, link tokens, passkey
    /// credentials, certificates, and DNS names, and revokes its key. The
    /// root node is never removed. On success the client stops the mesh and
    /// clears its session and node id. Local files (the identity key file,
    /// account image cache, certificate directory) stay; the caller deletes
    /// them (see Client SDK documentation). Response JSON: {success, status:
    /// "deleted" | "pending", account_id, removed_nodes?, op_id?}, or the
    /// server's "error". Requires an identity and an established mesh session.
    [[nodiscard]] Result<nlohmann::json> delete_account();

    /// Publish a local service to mesh peers: our mesh IP:vport bridges to
    /// `target` ("tcp:127.0.0.1:PORT"). Requires an active mesh (join first);
    /// exposures are remembered and re-applied on re-join. A port with a
    /// service catalog (set_service_catalog) is served through the SDK's
    /// ingress front end, which enforces the catalog's route policy; its
    /// target must be "tcp:<host>:<port>". Other ports bridge raw bytes.
    [[nodiscard]] StatusResult expose_service(uint16_t vport, const std::string& target);

    /// Exposures registered via expose_service ({vport, target} pairs).
    [[nodiscard]] std::vector<std::pair<uint16_t, std::string>> exposed_services() const;

    /// Register the route catalog of the service exposed on `vport`
    /// (service_ingress::parse_catalog gives the format and the rules),
    /// replacing any previous catalog of that port. From then on the SDK
    /// terminates HTTP at that port's ingress: it attributes each connection
    /// to the mesh peer it came from, matches method and path to a family,
    /// and forwards the request to the local service with the trusted
    /// X-Nexus-* headers only when the peer's listed permissions hold `admin`
    /// or `svc:<family>`. Every refusal answers 403 with a JSON body and is
    /// reported to the callback of set_ingress_denied_callback. Works before
    /// a join; an exposure already in force moves behind the front end at
    /// once. Errors: a validation failure (http_status 400, the message
    /// names the rule), or the forward could not be installed (503).
    [[nodiscard]] StatusResult set_service_catalog(uint16_t vport, const nlohmann::json& catalog);

    /// The catalog registered for `vport`, as given, or {} when none.
    [[nodiscard]] nlohmann::json service_catalog(uint16_t vport) const;

    /// Callback type for ingress refusals.
    using IngressDeniedCallback = ServiceIngress::DeniedCallback;

    /// Set the callback that runs for every request an ingress front end
    /// refuses (empty clears it): an unregistered route, a peer without the
    /// family permission, or a connection no listed peer accounts for. It
    /// runs on an SDK thread while the refused request waits, so it must
    /// return promptly. When this returns, the previous callback is not
    /// running and is not called again. Do not call this method, or destroy
    /// the client, from the callback. The event never carries a request body.
    void set_ingress_denied_callback(IngressDeniedCallback cb);

    /// Open (or reuse) a loopback bridge to a mesh peer's dst_ip:dst_port.
    /// Returns the bound 127.0.0.1 port, or 0 when the mesh is inactive.
    [[nodiscard]] uint16_t open_egress(const std::string& dst_ip, uint16_t dst_port);

    /// Close the loopback bridge to dst_ip:dst_port and abort its connections.
    void close_egress(const std::string& dst_ip, uint16_t dst_port);

private:
    /// POST /api/certs/issue with this identity's key and `fields` added to
    /// the request body.
    [[nodiscard]] Result<IssuedCertBundle> issue_certificate(const nlohmann::json& fields);

    /// Request and decrypt one certificate for a renewal target.
    [[nodiscard]] Result<DecryptedCert> fetch_certificate(const std::string& target);

    /// One session move, on the SDK's move thread: verify the membership
    /// again, then authenticate and join through another verified Tier 1
    /// server, or drop the session when that is impossible. `hint_epoch` is
    /// the next epoch a departure notice named (0 when none did). A `forced`
    /// move (the session's server stopped answering over the mesh) joins
    /// again even when that server is still a verified member reachable
    /// over the public API, through any verified member including the same
    /// one. When nothing can be verified, the session stays.
    void run_session_move(const std::string& reason, uint64_t hint_epoch, bool forced);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
