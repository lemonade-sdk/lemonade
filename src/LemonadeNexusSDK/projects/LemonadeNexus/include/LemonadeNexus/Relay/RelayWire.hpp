#pragma once

/// Relay wire encoding shared by the relay (RelayService) and the client SDK.
///
/// Every relay datagram starts with the 26-byte RelayPacketHeader. Multi-byte
/// header fields are little-endian.
///
/// Allocate (client to relay). The header session_id is zero. The payload is:
///
///     rendezvous_id[16] | proof[16] | peer_commitment[32] | ticket_json
///
/// `ticket_json` is the relay ticket in the JSON form that
/// `POST /api/relay/ticket` returns. It is 1 to kRelayMaxTicketJsonBytes bytes.
///
/// Allocated (relay to client). The header session_id is the session. The
/// payload is `state[1] | idle_timeout_sec[4]` (RelayAllocationState).
///
/// Bound (relay to client). Sent to the side that waited when the other side
/// joins the session. No payload.
///
/// Error (relay to client). The header echoes the request's session_id and
/// sequence number. The payload is one RelayErrorCode byte.
///
/// Data (client to relay) carries one BoringTun datagram as its payload. The
/// relay sends the payload, without the header, to the other bound side.
///
/// Rendezvous. Two peers that want one relay session derive the same
/// rendezvous id independently. Each side holds its own mesh private key and
/// the other side's mesh public key, so both compute the same X25519 shared
/// secret. Nobody else can. From that secret, the relay id, the two public
/// keys, and a time window, each side derives the rendezvous id and one proof
/// per side (the side with the lower public key is "low"). A side sends its own
/// proof and a commitment to the other side's proof. The relay joins two
/// Allocate requests only when each one opens the commitment the other one
/// made. Knowing the rendezvous id alone does not let a third party join a
/// session.

#include <LemonadeNexus/Relay/RelayTypes.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::relay {

/// Domain of the rendezvous derivation.
inline constexpr std::string_view kRelayRendezvousDomain = "lemonade-nexus/relay-rendezvous:v1";

/// Domain of a proof commitment.
inline constexpr std::string_view kRelayProofCommitDomain = "lemonade-nexus/relay-proof-commit:v1";

/// Length of one rendezvous time window, in seconds. Both sides must derive in
/// the same window; a side that misses retries in the next one.
inline constexpr uint64_t kRelayRendezvousWindowSec = 300;

/// Largest ticket JSON an Allocate request may carry, in bytes.
inline constexpr std::size_t kRelayMaxTicketJsonBytes = 1024;

/// Fixed part of an Allocate payload: rendezvous id, proof, peer commitment.
inline constexpr std::size_t kRelayAllocateFixedBytes = 16 + 16 + 32;

/// Longest peer_id or relay_id a ticket may name on the wire, in bytes.
inline constexpr std::size_t kRelayMaxIdBytes = 256;

using RelayProof = std::array<uint8_t, 16>;
using RelayCommitment = std::array<uint8_t, 32>;
using RendezvousId = std::array<uint8_t, 16>;

/// Why a relay refused a request. Sent as the one-byte Error payload.
enum class RelayErrorCode : uint8_t {
    TicketRefused    = 1,  ///< The ticket failed RelayTicketAuth checks.
    TicketReplayed   = 2,  ///< The ticket already allocated a session.
    Malformed        = 3,  ///< The request did not parse.
    RendezvousBusy   = 4,  ///< The rendezvous slot is held by another endpoint.
    ProofMismatch    = 5,  ///< The proof does not open the other side's commitment.
    CapacityExceeded = 6,  ///< A session, holder, or replay-set limit is reached.
    RateLimited      = 7,  ///< Too many Allocate requests; retry later.
    SessionUnknown   = 8,  ///< No session, or the sender is not bound to it.
    BindFailed       = 9,  ///< The bind token was invalid or already used.
};

/// The log name of a relay error code: the enumerator name, or "Unknown"
/// when the wire carries a value this build does not define.
[[nodiscard]] std::string_view relay_error_code_name(uint8_t code);

/// The session state an Allocated reply reports.
enum class RelayAllocationState : uint8_t {
    Waiting = 1,  ///< This side is bound; the other side has not joined yet.
    Paired  = 2,  ///< Both sides are bound; Data is forwarded.
};

/// What one side sends in its Allocate request, derived from the shared secret.
struct RelayRendezvous {
    RendezvousId    id{};
    RelayProof      own_proof{};
    RelayCommitment peer_commitment{};
};

/// A decoded Allocate payload.
struct RelayAllocateRequest {
    RendezvousId    rendezvous{};
    RelayProof      proof{};
    RelayCommitment peer_commitment{};
    RelayTicket     ticket;
};

/// A decoded Allocated payload.
struct RelayAllocatedReply {
    RelayAllocationState state{RelayAllocationState::Waiting};
    uint32_t             idle_timeout_sec{0};
};

/// The window index for `unix_sec`.
[[nodiscard]] constexpr uint64_t relay_rendezvous_window(uint64_t unix_sec) {
    return unix_sec / kRelayRendezvousWindowSec;
}

/// SHA-256 commitment to a proof, under kRelayProofCommitDomain.
[[nodiscard]] RelayCommitment relay_proof_commitment(const RelayProof& proof);

/// Derive one side's rendezvous material.
///
/// @param shared_secret X25519(own private key, peer public key). Must not be
///                      all zero.
/// @param own_pk, peer_pk Raw 32-byte X25519 public keys. Must differ.
/// @param relay_id      The relay the session is for (the ticket's relay_id).
/// @param window        relay_rendezvous_window() of the current time.
/// @return nullopt when the inputs are invalid. Both sides get the same `id`;
///         each side's `own_proof` opens the other side's `peer_commitment`.
[[nodiscard]] std::optional<RelayRendezvous> derive_relay_rendezvous(
    std::span<const uint8_t, 32> shared_secret, const std::array<uint8_t, 32>& own_pk,
    const std::array<uint8_t, 32>& peer_pk, std::string_view relay_id, uint64_t window);

/// The ticket in the JSON form of `POST /api/relay/ticket`.
[[nodiscard]] std::string relay_ticket_to_json(const RelayTicket& ticket);

/// Parse a ticket from that JSON form. Every field must be present, with the
/// exact decoded length. nullopt on any error; never throws.
[[nodiscard]] std::optional<RelayTicket> relay_ticket_from_json(std::string_view json_text);

/// Encode a header (little-endian fields) into `out`, which must hold
/// kRelayHeaderSize bytes.
void encode_relay_header(const RelayPacketHeader& header, std::span<uint8_t> out);

/// Decode a header. nullopt when the datagram is shorter than a header, the
/// magic or version is wrong, or payload_length exceeds the bytes present.
[[nodiscard]] std::optional<RelayPacketHeader> decode_relay_header(std::span<const uint8_t> datagram);

/// A complete datagram: header plus payload. The payload must fit in 16 bits.
[[nodiscard]] std::vector<uint8_t> make_relay_datagram(RelayMsgType type, const SessionId& session,
                                                       uint32_t sequence,
                                                       std::span<const uint8_t> payload = {});

/// A complete Allocate datagram. Empty when the ticket JSON is empty or larger
/// than kRelayMaxTicketJsonBytes.
[[nodiscard]] std::vector<uint8_t> make_relay_allocate(const RelayRendezvous& rendezvous,
                                                       std::string_view ticket_json,
                                                       uint32_t sequence);

/// Decode an Allocate payload (the bytes after the header). nullopt on any
/// error; never throws.
[[nodiscard]] std::optional<RelayAllocateRequest> decode_relay_allocate(
    std::span<const uint8_t> payload);

/// Encode an Allocated payload.
[[nodiscard]] std::array<uint8_t, 5> encode_relay_allocated(const RelayAllocatedReply& reply);

/// Decode an Allocated payload. nullopt when it is short or the state is unknown.
[[nodiscard]] std::optional<RelayAllocatedReply> decode_relay_allocated(
    std::span<const uint8_t> payload);

}  // namespace nexus::relay
