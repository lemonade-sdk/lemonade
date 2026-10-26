#pragma once

#include <LemonadeNexusSDK/CertRenewal.hpp>
#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Identity.hpp>
#include <LemonadeNexusSDK/LatencyMonitor.hpp>
#include <LemonadeNexusSDK/RoutingTypes.hpp>
#include <LemonadeNexusSDK/Tier1Directory.hpp>
#include <LemonadeNexusSDK/Types.hpp>
#include <LemonadeNexusSDK/BoringtunMesh.hpp>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lnsdk {

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

    /// GET /api/health. With a session, it also fails when the session's
    /// server is no longer a verified Tier 1 server, so the caller can join
    /// again through another one.
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

    /// Get mesh tunnel status including all peers with live stats.
    [[nodiscard]] MeshTunnelStatus mesh_status() const;

    /// Get the current list of known mesh peers.
    [[nodiscard]] std::vector<MeshPeer> get_mesh_peers() const;

    /// Force an immediate peer refresh (fetch from server + sync tunnel).
    void refresh_mesh_peers();

    /// Callback type for mesh state changes.
    using MeshStateCallback = std::function<void(const MeshTunnelStatus&)>;

    /// Set a callback to be notified on mesh state changes.
    void set_mesh_callback(MeshStateCallback cb);

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

    /// Set a single-use device link token to include in the next join_network
    /// call (obtained out-of-band from an already-joined device).
    void set_link_token(const std::string& token);

    /// Join as an inference node (Tier 3) instead of a client (Tier 4). Sent
    /// with the next join_network; the server records it when it creates the
    /// endpoint and derives the routing identifier from it ("infer-..." vs
    /// "client-..."). Default false.
    void set_inference_node(bool inference);

    /// Mint a single-use link token (an invite) for a target group (requires
    /// an established mesh session; calls the private API). An empty
    /// `target_node_id` targets this account's own group. An empty
    /// `permissions` list lets the server choose the default ("read",
    /// "connect_private", reduced to what the caller holds). A new device
    /// joins under the target with the token (set_link_token + join_network);
    /// an account that already joined becomes a member (accept_invite). The
    /// server refuses a target where the caller lacks add_child, and any
    /// permission the caller does not hold or an invite cannot grant.
    /// Response JSON: {link_token, group_node_id, target_node_id, permissions,
    /// expires_at}.
    [[nodiscard]] Result<nlohmann::json> create_link_token(
            uint32_t ttl_sec = 600, const std::string& target_node_id = {},
            const std::vector<std::string>& permissions = {});

    /// Accept an invite as an account that already joined: this identity
    /// becomes a member of the token's target group with the token's
    /// permissions. Nothing this account owns moves. The token is single use.
    /// Response JSON: {success, node_id, permissions, status: "member" |
    /// "pending", op_id?}. A refusal carries the server's "error".
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
    /// exposures are remembered and re-applied on re-join.
    [[nodiscard]] StatusResult expose_service(uint16_t vport, const std::string& target);

    /// Exposures registered via expose_service ({vport, target} pairs).
    [[nodiscard]] std::vector<std::pair<uint16_t, std::string>> exposed_services() const;

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

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
