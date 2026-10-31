#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lnsdk {

// ---------------------------------------------------------------------------
// Server configuration
// ---------------------------------------------------------------------------

/// A single server endpoint. `host` should be the server's certificate FQDN —
/// the public API is verified HTTPS by default and the cert can't be verified
/// against a bare IP.
struct ServerEndpoint {
    std::string host{"127.0.0.1"};
    uint16_t    port{9100};
    bool        use_tls{true};
};

struct ServerConfig {
    /// Seed list of servers (at least one). Each host must be a cert FQDN.
    /// Seeds are where the client starts: discovery, health, and the Tier 1
    /// membership walk. A seed that is not a verified Tier 1 server is never
    /// sent a tree request.
    std::vector<ServerEndpoint> servers{{"127.0.0.1", 9100, true}};

    int  connect_timeout_sec{5};
    int  read_timeout_sec{10};
    int  write_timeout_sec{5};

    /// If true, fetch /api/servers on first connect to discover additional servers.
    bool     auto_discover{true};

    /// If true, keep talking to the configured endpoint (an FQDN) after join
    /// instead of switching the public API to the server's advertised SEIP FQDN.
    /// The pinned host must still be a cert FQDN; pin an address separately if
    /// needed (verification is always on). It never applies to tree traffic:
    /// session, join, and tree requests go only to verified Tier 1 servers
    /// (LemonadeNexusClient::set_genesis_key).
    bool     pin_server{false};

    /// How often to check server health (seconds). 0 = disabled.
    uint32_t health_check_interval_sec{30};

    /// Convenience: single-server constructor for backward compat.
    ServerConfig() = default;
    ServerConfig(const std::string& host, uint16_t port, bool tls = true)
        : servers{{host, port, tls}} {}
};

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

struct HealthStatus {
    std::string status;
    std::string service;
    std::string dns_base_domain;
    std::string rp_id;  ///< WebAuthn relying-party id the network's passkeys are bound to; empty when the server did not report one
};

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

enum class AuthMethod : uint8_t {
    Ed25519,    // Primary: Ed25519 challenge-response identity auth
    Passkey,    // Backup: WebAuthn/FIDO2 passkey
    Password,   // Deprecated: stub on server (always fails)
    TokenLink,  // Deprecated: stub on server (always fails)
};

struct AuthRequest {
    AuthMethod  method{AuthMethod::Ed25519};
    std::string username;
    std::string password;
    std::string token;              // for token-link
    nlohmann::json passkey_data;    // for passkey/fido2
};

struct AuthResponse {
    bool        authenticated{false};
    std::string user_id;
    std::string session_token;
    std::string error;
};

struct PasskeyRegistration {
    std::string    user_id;
    nlohmann::json credential_data;
};

// ---------------------------------------------------------------------------
// Tree types — self-contained copies matching server's wire format
// ---------------------------------------------------------------------------

enum class NodeType : uint8_t {
    Root,
    Customer,
    Endpoint,
    Relay,
};

struct Assignment {
    std::string              management_pubkey; // "ed25519:base64..."
    std::vector<std::string> permissions;       // ["read","write","add_child",...]
};

struct TreeNode {
    std::string              id;
    std::string              parent_id;
    NodeType                 type{NodeType::Customer};
    std::string              hostname;

    // Network allocations
    std::string              tunnel_ip;
    std::string              private_subnet;
    std::string              private_shared_addresses;
    std::string              shared_domain;

    // Crypto
    std::string              mgmt_pubkey;
    std::string              wrapped_mgmt_privkey;
    std::string              mesh_pubkey;

    // Endpoint identity, as the server records it. is_inference marks a
    // Tier 3 inference node; false is a Tier 4 client.
    std::string              endpoint_identifier;
    std::string              cpu_id;
    std::string              net_mac;
    bool                     is_inference{false};

    // Assignments
    std::vector<Assignment>  assignments;

    // Signature by parent's management key
    std::string              signature;

    // Relay-specific fields
    std::string              listen_endpoint;
    std::string              region;
    uint32_t                 capacity_mbps{0};
    float                    reputation_score{1.0f};
    uint64_t                 expires_at{0};
};

struct TreeDelta {
    std::string operation;        // "create_node", "update_node", "delete_node", etc.
    std::string target_node_id;
    TreeNode    node_data;
    std::string signer_pubkey;
    std::string signature;
    uint64_t    timestamp{0};
};

struct DeltaResult {
    bool        success{false};
    uint64_t    delta_sequence{0};
    std::string node_id;
    std::string tunnel_ip;
    std::string private_subnet;
    std::string error;
};

/// A device of a group whose name folds to the same mesh-port label as a
/// name that was checked (LemonadeNexusClient::check_device_name).
struct DeviceNameConflict {
    std::string node_id;
    std::string device_name;
};

/// The server's answer to a device-name check: the name as a mesh-port label,
/// whether a device of the group could take it now, the devices it collides
/// with, and a label that is free (the label itself, or "<label>-2", ...).
/// Advisory: the join and rename operations enforce the rule.
struct DeviceNameCheck {
    std::string                     label;
    bool                            available{false};
    std::vector<DeviceNameConflict> conflicts;
    std::string                     suggestion;
};

// ---------------------------------------------------------------------------
// IPAM
// ---------------------------------------------------------------------------

enum class BlockType : uint8_t {
    Tunnel,
    Private,
    Shared,
};

struct AllocationRequest {
    std::string node_id;
    BlockType   block_type{BlockType::Tunnel};
    uint8_t     prefix_len{30};
};

struct AllocationResponse {
    bool        success{false};
    std::string network;
    std::string node_id;
};

// ---------------------------------------------------------------------------
// Relay
// ---------------------------------------------------------------------------

struct RelayNodeInfo {
    std::string relay_id;
    std::string public_key;
    std::string endpoint;
    std::string region;
    uint32_t    capacity_mbps{0};
    float       reputation_score{0.0f};
    float       estimated_latency_ms{0.0f};
    bool        supports_stun{true};
    bool        supports_relay{true};
    bool        is_central{false};
};

/// A relay ticket a Tier 1 server issued for this session. Any relay, Tier 1
/// or Tier 2, verifies it against the Tier 1 membership it verified.
struct RelayTicket {
    std::string peer_id;
    std::string relay_id;
    std::string session_nonce;     // base64
    uint64_t    issued_at{0};
    uint64_t    expires_at{0};
    std::string network_id;        // hex
    uint64_t    epoch{0};          // the issuer's Tier 1 epoch
    std::string issuer;            // base64 node identity key of the issuer
    std::string signature;         // base64
};

struct RelayRegistration {
    std::string relay_id;
    std::string endpoint;
    std::string region;
    std::string public_key;
    uint32_t    capacity_mbps{0};
    bool        supports_stun{true};
    bool        supports_relay{true};
};

struct RelayRegisterResult {
    bool        success{false};
    std::string relay_id;
};

// ---------------------------------------------------------------------------
// Certificates
// ---------------------------------------------------------------------------

struct CertStatus {
    std::string domain;
    bool        has_cert{false};
    uint64_t    expires_at{0};
};

/// An issued certificate bundle (borrowed license from server).
/// The private key is encrypted with the client's Ed25519 key via
/// ephemeral X25519 DH + HKDF + XChaCha20-Poly1305-IETF.
struct IssuedCertBundle {
    std::string domain;             ///< e.g. "my-laptop.capi.lemonade-nexus.io"
    std::string fullchain_pem;      ///< Full certificate chain (PEM)
    std::string encrypted_privkey;  ///< encrypted private key (base64)
    unsigned    crypto_version{0};  ///< crypto format version; 1 = XChaCha20-Poly1305-IETF
    std::string nonce;              ///< 24-byte nonce (base64)
    std::string ephemeral_pubkey;   ///< Server's ephemeral X25519 pubkey (base64)
    uint64_t    expires_at{0};      ///< Certificate expiry (Unix timestamp)
};

/// A decrypted certificate ready for use.
struct DecryptedCert {
    std::string domain;
    std::string fullchain_pem;
    std::string privkey_pem;
    uint64_t    expires_at{0};
};

// ---------------------------------------------------------------------------
// High-level results
// ---------------------------------------------------------------------------

struct JoinResult {
    bool        success{false};
    std::string node_id;
    std::string tunnel_ip;
    std::string private_subnet;
    std::string mesh_pubkey;        ///< mesh public key (base64)
    std::string error;
    /// The device name set with set_device_name was taken in the target group,
    /// so the device joined under the server's default name.
    bool        device_name_refused{false};
    /// A free name for a rename after a refused device name ("" when unknown).
    std::string device_name_suggestion;
    /// An invite this join carried for a device that already had a node. The
    /// device stays in its own network; the invite makes its identity a member
    /// of the invite's group instead. Empty status when the join carried no
    /// invite, or placed a new device with it.
    struct Membership {
        std::string status;    ///< "member", "pending", or "refused"
        std::string group_id;  ///< the invite's group
        std::string error;     ///< the server's reason when refused
    } membership;
};

// ---------------------------------------------------------------------------
// Passkey credential (for registration)
// ---------------------------------------------------------------------------

struct PasskeyCredential {
    std::string credential_id;   // base64url
    std::string public_key_x;   // hex, 32-byte P-256 X coordinate
    std::string public_key_y;   // hex, 32-byte P-256 Y coordinate
};

// ---------------------------------------------------------------------------
// Group membership
// ---------------------------------------------------------------------------

struct GroupMember {
    std::string              management_pubkey; // "ed25519:base64..."
    std::vector<std::string> permissions;       // ["read","write","add_child",...]
};

/// One device of a group, as GET /api/tree/devices/{group} lists it.
struct NetworkDevice {
    std::string node_id;
    std::string device_name;
    std::string tunnel_ip;     ///< "10.x.y.z/32"
    std::string mgmt_pubkey;   ///< "ed25519:base64..."
    std::string parent_id;     ///< the group the device is placed in
    std::string group_id;      ///< the group this entry was listed for
    /// How this identity belongs to group_id: "home" (its own network) or
    /// "member" (a network it joined with an invite). Set by
    /// list_network_devices; empty from get_group_devices.
    std::string group_role;
    bool is_inference{false};  ///< a Lemonade inference server
    /// The device joined group_id from its own network through an invite;
    /// it is placed in parent_id, outside group_id.
    bool member{false};
    bool is_self{false};       ///< this device
    bool can_connect{false};   ///< this device may connect to it (the peer-list rule)
    bool admin{false};         ///< its identity administers group_id
};

struct GroupJoinResult {
    bool        success{false};
    std::string node_id;
    std::string tunnel_ip;
    std::string parent_node_id;
    std::string error;
};

// ---------------------------------------------------------------------------
// Mesh tunnel
// ---------------------------------------------------------------------------

/// Config for the userspace boringtun mesh dataplane (Noise session to the
/// server + cryptokey routing). No kernel interface — see BoringtunMesh.
struct BoringtunConfig {
    std::string private_key;                ///< Our Curve25519 private key (base64)
    std::string public_key;                 ///< Our Curve25519 public key (base64)
    std::string tunnel_ip;                  ///< Assigned mesh IP (e.g. "10.64.0.5/32")
    std::string server_public_key;          ///< Server's Curve25519 public key
    std::string server_endpoint;            ///< Server endpoint "host:port"
    std::string dns_server;                 ///< DNS server IP (optional)
    uint16_t    listen_port{0};             ///< Local UDP port (0 = random)
    std::vector<std::string> allowed_ips;   ///< Mesh plane, e.g. ["10.64.0.0/10"]
    uint32_t    keepalive{5};               ///< Persistent keepalive interval (seconds)
    /// The session server's in-mesh address (e.g. "10.64.252.37"), where its
    /// private API answers. No mesh peer may claim it (see
    /// BoringtunMesh::peer_allowed_ips). Empty when unknown.
    std::string server_tunnel_ip;
};

struct TunnelStatus {
    bool        is_up{false};
    std::string tunnel_ip;
    std::string server_endpoint;
    int64_t     last_handshake{0};          ///< Unix timestamp of last Noise handshake
    uint64_t    rx_bytes{0};
    uint64_t    tx_bytes{0};
    int32_t     latency_ms{-1};             ///< -1 = unknown
};

// ---------------------------------------------------------------------------
// Mesh P2P networking
// ---------------------------------------------------------------------------

/// A single mesh peer for P2P connections.
struct MeshPeer {
    std::string node_id;
    std::string hostname;
    std::string mesh_pubkey;            ///< Curve25519 base64
    std::string tunnel_ip;              ///< e.g. "10.64.0.5/32"
    std::string private_subnet;         ///< e.g. "10.128.17.4/30"
    std::string endpoint;               ///< Direct "ip:port" (from STUN/hole-punch)
    /// The peer's LAN endpoints ("a.b.c.d:port", private addresses) that this
    /// device probes: the server lists them from the peer's heartbeats, and
    /// the mesh orchestrator keeps them only when the peer is on this
    /// device's network (lan_probe_targets). Empty otherwise.
    std::vector<std::string> local_endpoints;
    std::string relay_endpoint;         ///< Relay fallback "ip:port" (empty if not needed)
    /// The path the peer's traffic takes now, as the mesh orchestrator's
    /// last monitor pass found it: "lan" (one of local_endpoints), "direct"
    /// (another address, normally the punched public endpoint), "relay", or
    /// "" (no working path yet, or the path went silent).
    std::string path;
    bool        is_online{false};
    int64_t     last_handshake{0};      ///< Unix timestamp
    /// Unix timestamp of the last authenticated packet from the peer,
    /// keepalives included (0 = none yet). Keepalives do not renew the
    /// handshake, so this is the liveness signal.
    int64_t     last_rx{0};
    uint64_t    rx_bytes{0};
    uint64_t    tx_bytes{0};
    int32_t     latency_ms{-1};         ///< -1 = unknown
    uint16_t    keepalive{5};           ///< Persistent keepalive (seconds)
    uint64_t    last_seen{0};           ///< Server-reported last heartbeat (Unix epoch)
    /// The peer's management key ("ed25519:..."), when the server lists it.
    std::string mgmt_pubkey;
    /// The permissions the peer's key holds on this device and its ancestor
    /// groups ("admin", "svc:<family>", ...), as the server lists them in
    /// peer_permissions. The service ingress (ServiceIngress) decides from
    /// them which route families the peer may call. Empty grants nothing.
    std::vector<std::string> permissions;
};

/// Status of the entire mesh tunnel.
struct MeshTunnelStatus {
    bool        is_up{false};
    std::string tunnel_ip;
    uint32_t    peer_count{0};
    uint32_t    online_count{0};
    uint64_t    total_rx_bytes{0};
    uint64_t    total_tx_bytes{0};
    std::vector<MeshPeer> peers;
    /// True when the Tier 1 server that issued the session left the verified
    /// membership and the SDK could not move the session by itself: the
    /// device holds no identity to authenticate with, or the silent join
    /// failed. The session is dropped and every private-API call is refused
    /// until join_network() binds a new session; a successful join clears
    /// it. Reported by LemonadeNexusClient::mesh_status(), not by the mesh
    /// state callback. A session the SDK moved by itself never sets it.
    bool        needs_rejoin{false};
};

/// One session move: the Tier 1 server that issued the session left the
/// verified membership, announced that it leaves at the next epoch, or
/// stopped answering over the mesh while its public API answered, and the
/// SDK bound the session to a verified Tier 1 server again (another one, or
/// the same one after a mesh outage)
/// (LemonadeNexusClient::set_session_moved_callback).
struct SessionMoveEvent {
    std::string from_node_id;  ///< base64 node id of the server that issued the old session
    std::string from_host;     ///< its certificate FQDN, when it was still verified
    std::string to_node_id;    ///< the server that issued the new session; empty when none did
    std::string to_host;       ///< its certificate FQDN
    /// The Tier 1 epoch the SDK verified when it moved the session.
    uint64_t    epoch{0};
    /// True when the SDK authenticated with the device identity and joined
    /// again by itself; the application's view is intact. False when the
    /// application must call join_network(): the session was dropped and
    /// MeshTunnelStatus::needs_rejoin is set.
    bool        silent{false};
    /// Why no silent move happened; empty when `silent` is true.
    std::string error;
};

/// Configuration for the mesh orchestrator.
struct MeshConfig {
    uint32_t peer_refresh_interval_sec{30};  ///< How often to poll for peer updates
    uint32_t heartbeat_interval_sec{5};      ///< How often to send heartbeat
    /// How often to learn and publish the reflexive address (STUN query from
    /// the dataplane socket to the session server). 0 turns it off.
    uint32_t stun_refresh_interval_sec{60};
    /// Prefer direct P2P over relay: keep probing a relayed peer's direct
    /// endpoint and leave the relay when a probe gets a handshake. False puts
    /// every peer on a relay when one is available.
    bool     prefer_direct{true};
    bool     auto_connect{true};             ///< Auto-connect to discovered peers
};

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

struct StatsResponse {
    std::string service;
    uint32_t    peer_count{0};
    bool        private_api_enabled{false};
};

// ---------------------------------------------------------------------------
// Server discovery
// ---------------------------------------------------------------------------

struct ServerEntry {
    std::string endpoint;
    std::string pubkey;
    uint16_t    http_port{0};
    uint64_t    last_seen{0};
    bool        healthy{false};
};

// ---------------------------------------------------------------------------
// DDNS
// ---------------------------------------------------------------------------

struct DdnsStatus {
    bool        has_credentials{false};
    std::string last_ip;
    std::string binary_hash;
    bool        binary_approved{false};
};

// ---------------------------------------------------------------------------
// Attestation manifests
// ---------------------------------------------------------------------------

struct AttestationManifest {
    std::string version;
    std::string platform;
    std::string binary_sha256;
    uint64_t    timestamp{0};
};

struct AttestationManifests {
    std::string              self_hash;
    bool                     self_approved{false};
    std::vector<AttestationManifest> manifests;
    std::string              github_url;
    std::string              minimum_version;
    uint32_t                 manifest_fetch_interval_sec{0};
};

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------

[[nodiscard]] std::string node_type_to_string(NodeType type);
[[nodiscard]] NodeType string_to_node_type(std::string_view s);

void to_json(nlohmann::json& j, const Assignment& a);
void from_json(const nlohmann::json& j, Assignment& a);
void to_json(nlohmann::json& j, const TreeNode& n);
void from_json(const nlohmann::json& j, TreeNode& n);
void to_json(nlohmann::json& j, const TreeDelta& d);
void from_json(const nlohmann::json& j, TreeDelta& d);

/// Produce a canonical (deterministic) JSON string for Ed25519 signing.
/// Excludes the "signature" field itself.
[[nodiscard]] std::string canonical_delta_json(const TreeDelta& delta);

void to_json(nlohmann::json& j, const SessionMoveEvent& e);
void from_json(const nlohmann::json& j, StatsResponse& s);
void from_json(const nlohmann::json& j, ServerEntry& s);
void from_json(const nlohmann::json& j, DdnsStatus& s);
void from_json(const nlohmann::json& j, AttestationManifest& m);
void from_json(const nlohmann::json& j, AttestationManifests& a);

void to_json(nlohmann::json& j, const MeshPeer& p);
void from_json(const nlohmann::json& j, MeshPeer& p);
void to_json(nlohmann::json& j, const MeshTunnelStatus& s);
void from_json(const nlohmann::json& j, MeshTunnelStatus& s);
void to_json(nlohmann::json& j, const MeshConfig& c);
void from_json(const nlohmann::json& j, MeshConfig& c);

} // namespace lnsdk
