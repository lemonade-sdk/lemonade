#pragma once

/// PCP client (RFC 6887), the successor of NAT-PMP. A device behind a home
/// NAT asks its default gateway for a public UDP port that forwards to the
/// dataplane port, so a peer behind a symmetric NAT can dial it. The codec
/// functions are pure and testable without a network; PcpClient carries
/// them over UDP. Only the MAP opcode over IPv4 is implemented. A gateway
/// that answers the version fallback of RFC 6887 section 9 is reported
/// through PcpClient::unsupported_version(), and the caller then tries
/// NAT-PMP (NatPmp.hpp).
///
/// Follow-up: listening for multicast ANNOUNCE responses on 224.0.0.1:5350
/// (RFC 6887 section 14.1), which tell a client about a gateway restart
/// before its next renewal, is not implemented. A restart is only detected
/// through epoch_indicates_restart() at the next exchange.

#include <LemonadeNexusSDK/Error.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lnsdk::pcp {

/// The UDP port a PCP server listens on (RFC 6887 section 19.1).
inline constexpr uint16_t kDefaultPort = 5351;

/// Protocol version in the first byte of every request and response.
inline constexpr uint8_t kVersion = 2;

/// The MAP opcode. A response carries the request opcode with kResponseBit.
inline constexpr uint8_t kOpcodeMap = 1;
inline constexpr uint8_t kResponseBit = 0x80;

/// The IANA protocol number for UDP, the only protocol this client maps.
inline constexpr uint8_t kProtocolUdp = 17;

/// Wire sizes (RFC 6887 sections 7.1, 7.2, and 11.1): the common header,
/// and the header plus the MAP opcode data.
inline constexpr size_t kHeaderLen = 24;
inline constexpr size_t kMapRequestLen = 60;
inline constexpr size_t kMapResponseLen = 60;

/// The length of the mapping nonce (RFC 6887 section 11.1).
inline constexpr size_t kNonceLen = 12;
using Nonce = std::array<uint8_t, kNonceLen>;

/// Result codes of a response (RFC 6887 section 7.4).
enum class ResultCode : uint8_t {
    Success = 0,
    UnsuppVersion = 1,
    NotAuthorized = 2,
    MalformedRequest = 3,
    UnsuppOpcode = 4,
    UnsuppOption = 5,
    MalformedOption = 6,
    NetworkFailure = 7,
    NoResources = 8,
    UnsuppProtocol = 9,
    UserExQuota = 10,
    CannotProvideExternal = 11,
    AddressMismatch = 12,
    ExcessiveRemotePeers = 13,
};

/// The name of a result code, "Unknown" for a value outside the enumeration.
[[nodiscard]] const char* result_code_name(ResultCode code) noexcept;

/// The fields of a MAP request. Addresses and ports are in host byte order;
/// an IPv4 address is a 32-bit integer and goes on the wire as an
/// IPv4-mapped IPv6 address (::ffff:a.b.c.d).
struct MapRequest {
    uint32_t lifetime_sec{0};  ///< Requested lifetime; 0 deletes the mapping.
    /// The address the request leaves from. The server compares it with the
    /// source address of the datagram and answers ADDRESS_MISMATCH when
    /// they differ (RFC 6887 section 8.1).
    uint32_t client_address{0};
    Nonce nonce{};                      ///< Identifies the mapping; see PcpClient.
    uint8_t protocol{kProtocolUdp};     ///< IANA protocol number.
    uint16_t internal_port{0};          ///< The port on this device.
    uint16_t suggested_external_port{0};     ///< A preference; 0 for none.
    uint32_t suggested_external_address{0};  ///< A preference; 0 for none.
};

/// The fields of a MAP response, in host byte order. On an error result
/// the server echoes the request's nonce, protocol, and internal port, and
/// the other body fields are 0; a header-only error response has every body
/// field at 0.
struct MapResponse {
    ResultCode result{ResultCode::Success};
    uint32_t lifetime_sec{0};      ///< The granted lifetime; 0 after a delete.
    uint32_t epoch_sec{0};         ///< Seconds since the server's mapping table started.
    Nonce nonce{};                 ///< The request's nonce, echoed.
    uint8_t protocol{0};
    uint16_t internal_port{0};
    uint16_t external_port{0};     ///< The public port assigned; 0 after a delete.
    uint32_t external_address{0};  ///< The public IPv4 address; 0 after a delete.
};

/// The 60-byte MAP request for `request`: version 2, opcode 1 with the R
/// bit clear, reserved, lifetime, the client address as an IPv4-mapped IPv6
/// address, then the nonce, protocol, reserved, internal port, suggested
/// external port, and suggested external address (all zero when 0).
[[nodiscard]] std::vector<uint8_t> encode_map_request(const MapRequest& request);

/// The parsed MAP response. Accepts a 60-byte response, or a 24-byte
/// header-only response with an error result; returns nullopt for any other
/// length, a version other than 2, a clear R bit, an opcode other than MAP,
/// a 24-byte response with result Success, or an assigned external address
/// that is not an IPv4-mapped IPv6 address on a Success result. On an error
/// result an all-zero address decodes to 0. Reserved bytes are ignored.
[[nodiscard]] std::optional<MapResponse> decode_map_response(std::span<const uint8_t> bytes);

/// The server's version from the fallback reply of RFC 6887 section 9: a
/// response of at least 4 bytes with result code 1 (UNSUPP_VERSION) and a
/// version other than 2 in the first byte. A NAT-PMP-only gateway answers
/// with version 0 (its own unsupported-version response), and a future
/// server answers with the highest version it supports. Returns nullopt for
/// any other datagram.
[[nodiscard]] std::optional<uint8_t> decode_unsupported_version(std::span<const uint8_t> bytes);

/// Whether a server epoch shows that the server lost its mappings since the
/// last response (RFC 6887 section 8.5). `previous_epoch` came with a
/// response at `previous_time_unix`; `current_epoch` came at `now_unix`
/// (both Unix seconds). The server's time must advance together with the
/// client's: a decrease of the epoch, or a difference between the two
/// elapsed times beyond 1/16 of the elapsed time plus 2 seconds, means a
/// restart, and every mapping must be requested again. Client time that
/// moved backwards counts as no elapsed time. The NAT-PMP rule in
/// natpmp::epoch_indicates_restart() is one-sided and keeps RFC 6886's
/// tolerance, so the two are separate.
[[nodiscard]] bool epoch_indicates_restart(uint32_t previous_epoch, int64_t previous_time_unix,
                                           uint32_t current_epoch, int64_t now_unix);

/// Sends PCP MAP requests for one UDP mapping to one gateway.
///
/// Every call is synchronous: it opens a UDP socket bound to the local
/// address, sends the request, waits for the answer with retransmissions,
/// and closes the socket. A first attempt (map_udp) waits 1 s, 2 s, and
/// 4 s (three sends, 7 s in total; the RFC's longer schedule would stall
/// the caller); a renewal (renew_udp) and a delete wait 1 s and 1.5 s. Only
/// datagrams from the gateway address and port are read. A MAP response is
/// accepted when it carries the mapping's nonce; a header-only error
/// response for MAP is accepted because the server could not read the
/// nonce; any other datagram is ignored silently and the wait continues. An
/// error result ends the attempt with that code in last_result_code(). No
/// call throws.
///
/// One instance owns one mapping: map_udp() for a new internal port draws a
/// fresh random nonce and keeps it, and renew_udp(), release_udp(), and a
/// repeated map_udp() for the same port reuse it, because the server
/// identifies the mapping by (nonce, protocol, internal port) (RFC 6887
/// section 11.1). Calls write that state, so serialize calls on one
/// instance.
class PcpClient {
public:
    /// `gateway_ipv4` is the gateway's dotted IPv4 address; `local_ipv4` is
    /// the dotted address of this device on the interface that reaches it,
    /// as gateway discovery reported it (the socket binds to it and it goes
    /// into the request, which RFC 6887 section 8.1 requires to match);
    /// `port` is the gateway's PCP port (kDefaultPort; a test passes its
    /// fake's loopback port). An address that does not parse or is not
    /// local makes every call fail. `cancel`, when set, must outlive the
    /// client: a call in progress ends within a quarter second once another
    /// thread stores true in it, and fails with "PCP exchange cancelled".
    PcpClient(std::string gateway_ipv4, std::string local_ipv4, uint16_t port = kDefaultPort,
              const std::atomic<bool>* cancel = nullptr);

    /// Requests a public UDP port that forwards to `internal_port` (1 to
    /// 65535) for `lifetime_sec` seconds (1 or more; RFC 6887 recommends
    /// asking for at most 24 hours, and 7200 is the default elsewhere in the
    /// SDK). `suggested_external_port` is a preference the gateway may
    /// ignore; 0 means none. A new internal port replaces the mapping this
    /// instance holds; the same internal port is a renewal that keeps the
    /// nonce but uses the first-attempt schedule. On success the returned
    /// MapResponse holds the port, address, and lifetime the gateway
    /// granted, which can differ from the request. Fails when the gateway
    /// does not answer within the budget, answers an error result (named by
    /// last_result_code()), or answers the version fallback (named by
    /// unsupported_version()).
    [[nodiscard]] Result<MapResponse> map_udp(uint16_t internal_port,
                                              uint16_t suggested_external_port,
                                              uint32_t lifetime_sec);

    /// Renews the mapping this instance holds for `lifetime_sec` more
    /// seconds (1 or more) with the kept nonce and the granted external port
    /// as the suggestion, on the shorter schedule. Fails without sending
    /// when no mapping is held. Call it before the granted lifetime ends.
    [[nodiscard]] Result<MapResponse> renew_udp(uint32_t lifetime_sec);

    /// Deletes the mapping of `internal_port` (1 to 65535): a MAP request
    /// with lifetime 0, the kept nonce, and no suggestion (RFC 6887 section
    /// 15.1). Fails without sending when this instance holds no mapping for
    /// that port. Succeeds when the gateway answers Success, and forgets the
    /// mapping; a failure keeps it so the caller can retry.
    [[nodiscard]] StatusResult release_udp(uint16_t internal_port);

    /// The result code of the last response the gateway sent, nullopt when
    /// no call has received a response yet. UnsuppVersion after the version
    /// fallback.
    [[nodiscard]] std::optional<ResultCode> last_result_code() const noexcept;

    /// The epoch of the last response the gateway sent (0 before the first
    /// response). Compare with epoch_indicates_restart().
    [[nodiscard]] uint32_t last_epoch_sec() const noexcept;

    /// The version the gateway offered in a version fallback reply (0 for a
    /// NAT-PMP-only gateway), nullopt when no reply did. When set, the
    /// caller tries NAT-PMP instead of retrying PCP.
    [[nodiscard]] std::optional<uint8_t> unsupported_version() const noexcept;

    /// The nonce of the mapping this instance holds, nullopt when none.
    [[nodiscard]] std::optional<Nonce> nonce() const noexcept;

private:
    struct Held {
        Nonce nonce{};
        uint16_t internal_port{0};
        uint16_t external_port{0};
    };

    /// Sends `request` on `waits` and returns the accepted MAP response.
    Result<MapResponse> exchange(const MapRequest& request,
                                 std::span<const std::chrono::milliseconds> waits);

    std::string gateway_;
    std::string local_;
    uint16_t port_;
    const std::atomic<bool>* cancel_;
    std::optional<Held> held_;
    std::optional<ResultCode> last_result_code_;
    uint32_t last_epoch_sec_{0};
    std::optional<uint8_t> unsupported_version_;
};

}  // namespace lnsdk::pcp
