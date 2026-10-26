#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace nexus::relay {

// --- Protocol constants ---
static constexpr uint16_t kRelayMagic       = 0x4C52; // "LR"
static constexpr uint8_t  kRelayVersion     = 0x01;
static constexpr uint16_t kDefaultRelayPort = 9103;
static constexpr std::size_t kRelayHeaderSize = 26;

/// 128-bit session identifier.
using SessionId = std::array<uint8_t, 16>;

/// Hash functor for SessionId, allowing use in unordered containers.
struct SessionIdHash {
    std::size_t operator()(const SessionId& id) const noexcept {
        // FNV-1a over 16 bytes
        std::size_t hash = 14695981039346656037ULL;
        for (auto byte : id) {
            hash ^= static_cast<std::size_t>(byte);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

// --- Message types ---
enum class RelayMsgType : uint8_t {
    Allocate     = 0x01,
    Allocated    = 0x02,
    Bind         = 0x03,
    Bound        = 0x04,
    Data         = 0x10,
    Heartbeat    = 0x20,
    HeartbeatAck = 0x21,
    Teardown     = 0x30,
    Error        = 0xFF
};

// --- Wire format ---
#pragma pack(push, 1)
struct RelayPacketHeader {
    uint16_t     magic;
    uint8_t      version;
    RelayMsgType msg_type;
    SessionId    session_id;
    uint32_t     sequence_number;
    uint16_t     payload_length;
};
#pragma pack(pop)
static_assert(sizeof(RelayPacketHeader) == kRelayHeaderSize,
              "RelayPacketHeader must be exactly 26 bytes");

// --- Domain types ---

/// Ticket authorising a peer to use a relay for a session. A current Tier 1
/// member issues it and signs it with its node identity key; any relay
/// verifies it against its verified Tier 1 membership (RelayTicketAuth.hpp).
struct RelayTicket {
    std::string              peer_id;     ///< the session the ticket is for
    std::string              relay_id;
    std::array<uint8_t, 16>  session_nonce{};
    uint64_t                 issued_at{0};   ///< Unix seconds
    uint64_t                 expires_at{0};  ///< Unix seconds
    std::array<uint8_t, 32>  network_id{};   ///< the network the issuer serves
    uint64_t                 epoch{0};       ///< the epoch the issuer is a member of
    std::array<uint8_t, 32>  issuer{};       ///< the issuer's node identity key
    std::array<uint8_t, 64>  signature{};
};

/// Result of an Allocate request.
struct RelayAllocation {
    SessionId   session_id{};
    std::string relay_endpoint;
    uint32_t    ttl_seconds{300};
    std::string error_message;
};

/// Result of a Bind request.
struct RelayBindResult {
    bool        success{false};
    SessionId   session_id{};
    std::string error_message;
};

/// Per-session statistics.
struct RelaySessionStats {
    SessionId session_id{};
    uint64_t  bytes_forwarded{0};
    uint64_t  packets_forwarded{0};
    uint32_t  packets_dropped{0};
    float     avg_latency_ms{0.0f};
};

/// Quality report submitted by a peer after a session.
struct RelayQualityReport {
    std::string relay_id;
    float       packet_loss_rate{0.0f};
    float       avg_latency_ms{0.0f};
    float       jitter_ms{0.0f};
    uint64_t    bytes_transferred{0};
    uint32_t    session_duration_seconds{0};
    bool        session_completed_cleanly{true};
};

/// Descriptor for a relay node in the mesh.
struct RelayNodeInfo {
    std::string relay_id;
    std::string public_key;
    std::string endpoint;
    std::string region;            ///< Geographic region code (e.g. "us-ca", "eu-de")
    std::string hostname;          ///< DNS hostname for relay discovery
    uint32_t    capacity_mbps{0};
    float       reputation_score{0.0f};
    float       estimated_latency_ms{0.0f};
    bool        supports_stun{true};
    bool        supports_relay{true};
    bool        is_central{false};
};

/// Criteria used when selecting the best relay.
struct RelaySelectionCriteria {
    std::string preferred_region;
    float       min_reputation{0.5f};
    uint32_t    min_capacity_mbps{10};
    uint32_t    max_latency_ms{200};
    uint32_t    max_results{5};
};

} // namespace nexus::relay
