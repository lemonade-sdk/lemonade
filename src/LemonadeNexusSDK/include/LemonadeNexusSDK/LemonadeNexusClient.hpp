#pragma once

#include <LemonadeNexusSDK/Error.hpp>
#include <LemonadeNexusSDK/Identity.hpp>
#include <LemonadeNexusSDK/LatencyMonitor.hpp>
#include <LemonadeNexusSDK/RoutingTypes.hpp>
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
    // Health
    // -----------------------------------------------------------------

    /// GET /api/health
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

    /// Add a member to a group node's assignments.
    [[nodiscard]] Result<DeltaResult> add_group_member(const std::string& node_id,
                                                         const GroupMember& member);

    /// Remove a member from a group node's assignments by pubkey.
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

    /// Set the session token obtained from authentication.
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

    /// Mint a device link token so another device can join this account's
    /// group (requires an established mesh session; calls the private API).
    /// Response JSON: {link_token, group_node_id, expires_at}.
    [[nodiscard]] Result<nlohmann::json> create_link_token(uint32_t ttl_sec = 600);

    /// Publish a local service to mesh peers: our mesh IP:vport bridges to
    /// `target` ("tcp:127.0.0.1:PORT"). Requires an active mesh (join first);
    /// exposures are remembered and re-applied on re-join.
    [[nodiscard]] StatusResult expose_service(uint16_t vport, const std::string& target);

    /// Exposures registered via expose_service ({vport, target} pairs).
    [[nodiscard]] std::vector<std::pair<uint16_t, std::string>> exposed_services() const;

    /// Open (or reuse) a loopback bridge to a mesh peer's dst_ip:dst_port.
    /// Returns the bound 127.0.0.1 port, or 0 when the mesh is inactive.
    [[nodiscard]] uint16_t open_egress(const std::string& dst_ip, uint16_t dst_port);
    void close_egress(const std::string& dst_ip, uint16_t dst_port);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lnsdk
