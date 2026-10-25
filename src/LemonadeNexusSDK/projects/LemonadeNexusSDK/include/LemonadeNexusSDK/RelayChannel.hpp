#pragma once

/// One relay session that carries one peer's BoringTun traffic.
///
/// A RelayChannel owns a UDP socket. It sends the Allocate request to the
/// relay from that socket, so the relay binds the socket's public mapping.
/// After the relay pairs the session, the channel shuttles datagrams:
///   - from the local BoringTun dataplane (127.0.0.1:dataplane_port): wrapped
///     in a relay Data header and sent to the relay;
///   - from the relay: a raw BoringTun datagram, sent on to the dataplane.
/// The dataplane points the peer at local_endpoint(), so it needs no relay
/// support of its own. Every datagram from any other source is dropped.
///
/// The relay only carries ciphertext: the BoringTun session between the two
/// peers authenticates and encrypts every packet end to end.
///
/// Wire format and rendezvous: see LemonadeNexus/Relay/RelayWire.hpp.

#include <LemonadeNexusSDK/Types.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lnsdk {

class RelayChannel {
public:
    enum class State : uint8_t {
        Idle,        ///< Not opened.
        Allocating,  ///< Allocate sent; no reply yet.
        Waiting,     ///< This side is bound; the peer has not joined.
        Paired,      ///< Both sides bound; traffic flows.
        Failed,      ///< Ended; see failure().
    };

    enum class Failure : uint8_t {
        None,
        NoReply,      ///< The relay never answered the Allocate request.
        Refused,      ///< The relay refused the Allocate request.
        PeerAbsent,   ///< The peer did not join within the pending limit.
        RelaySilent,  ///< The relay stopped answering after allocation.
        SessionLost,  ///< The relay no longer knows the session.
        Socket,       ///< The local socket failed.
    };

    struct Options {
        std::string relay_ipv4;      ///< Dotted IPv4 address of the relay.
        uint16_t    relay_port{9103};
        uint16_t    dataplane_port{0};  ///< The local BoringTun UDP port.
        /// The complete Allocate datagram (relay_allocate_datagram).
        std::vector<uint8_t> allocate;
        std::chrono::milliseconds allocate_retry{1000};
        uint32_t                  allocate_attempts{5};
        std::chrono::milliseconds heartbeat_interval{10000};
        /// Fail when the relay sends nothing for this long after allocation.
        std::chrono::milliseconds relay_silence_limit{45000};
        /// Fail when the peer has not joined this long after allocation.
        std::chrono::milliseconds pending_limit{35000};
    };

    RelayChannel();
    /// Closes the channel.
    ~RelayChannel();

    RelayChannel(const RelayChannel&) = delete;
    RelayChannel& operator=(const RelayChannel&) = delete;

    /// Bind the socket, start the channel thread, and send the Allocate
    /// request. Returns false (and opens nothing) when the options are invalid
    /// or the socket cannot be bound. Call once.
    bool open(Options options);

    /// Tell the relay to end a known session, stop the thread, and close the
    /// socket. Idempotent. Thread-safe.
    void close();

    [[nodiscard]] State state() const;
    [[nodiscard]] Failure failure() const;

    /// "127.0.0.1:<port>": where the dataplane sends this peer's datagrams.
    /// Empty before open().
    [[nodiscard]] std::string local_endpoint() const;

    /// "<relay ip>:<port>".
    [[nodiscard]] std::string relay_endpoint() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The Allocate datagram for `ticket`, from 64 bytes of rendezvous material
/// (BoringtunMesh::relay_rendezvous: id, own proof, peer commitment). Empty
/// when the ticket does not encode.
[[nodiscard]] std::vector<uint8_t> relay_allocate_datagram(const std::array<uint8_t, 64>& rendezvous,
                                                           const RelayTicket& ticket,
                                                           uint32_t sequence);

/// Pick the relay for the pair of mesh public keys (base64) by highest
/// random weight: both peers pick the same relay from the same list, in any
/// order, and still agree when the lists differ in other entries. nullopt
/// when `relay_ids` is empty.
[[nodiscard]] std::optional<std::size_t> select_relay_for_pair(
    const std::vector<std::string>& relay_ids, const std::string& pubkey_a,
    const std::string& pubkey_b);

/// The first IPv4 address of `host` (a dotted address or a DNS name).
/// nullopt when it does not resolve. Blocks on DNS.
[[nodiscard]] std::optional<std::string> resolve_relay_ipv4(const std::string& host);

}  // namespace lnsdk
