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
    std::vector<ServerEndpoint> servers{{"127.0.0.1", 9100, true}};

    int  connect_timeout_sec{5};
    int  read_timeout_sec{10};
    int  write_timeout_sec{5};

    /// If true, fetch /api/servers on first connect to discover additional servers.
    bool     auto_discover{true};

    /// If true, keep talking to the configured endpoint (an FQDN) after join
    /// instead of switching the public API to the server's advertised SEIP FQDN.
    /// The pinned host must still be a cert FQDN; pin an address separately if
    /// needed (verification is always on).
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

struct RelayTicket {
    std::string peer_id;
    std::string relay_id;
    std::string session_nonce;     // base64
    uint64_t    issued_at{0};
    uint64_t    expires_at{0};
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
    std::string relay_endpoint;         ///< Relay fallback "ip:port" (empty if not needed)
    bool        is_online{false};
    int64_t     last_handshake{0};      ///< Unix timestamp
    uint64_t    rx_bytes{0};
    uint64_t    tx_bytes{0};
    int32_t     latency_ms{-1};         ///< -1 = unknown
    uint16_t    keepalive{5};           ///< Persistent keepalive (seconds)
    uint64_t    last_seen{0};           ///< Server-reported last heartbeat (Unix epoch)
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
};

/// Configuration for the mesh orchestrator.
struct MeshConfig {
    uint32_t peer_refresh_interval_sec{30};  ///< How often to poll for peer updates
    uint32_t heartbeat_interval_sec{5};      ///< How often to send heartbeat
    uint32_t stun_refresh_interval_sec{60};  ///< How often to re-probe STUN
    bool     prefer_direct{true};            ///< Prefer direct P2P over relay
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
