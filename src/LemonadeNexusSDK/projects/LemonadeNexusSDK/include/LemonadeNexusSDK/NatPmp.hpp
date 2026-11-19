#pragma once

/// NAT-PMP client (RFC 6886). A device behind a home NAT asks its default
/// gateway for its public address and for a public UDP port that forwards to
/// the dataplane port, so a peer behind a symmetric NAT can dial it. The
/// codec functions are pure and testable without a network; NatPmpClient
/// carries them over UDP.

#include <LemonadeNexusSDK/Error.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lnsdk::natpmp {

/// The UDP port a NAT-PMP gateway listens on (RFC 6886 section 3).
inline constexpr uint16_t kDefaultPort = 5351;

/// Protocol version in the first byte of every request and response.
inline constexpr uint8_t kVersion = 0;

/// Request opcodes. A response carries the request opcode plus kResponseBit.
inline constexpr uint8_t kOpExternalAddress = 0;
inline constexpr uint8_t kOpMapUdp = 1;
inline constexpr uint8_t kResponseBit = 128;

/// Wire sizes (RFC 6886 sections 3.2 and 3.3).
inline constexpr size_t kExternalAddressRequestLen = 2;
inline constexpr size_t kExternalAddressResponseLen = 12;
inline constexpr size_t kMapRequestLen = 12;
inline constexpr size_t kMapResponseLen = 16;

/// Result codes of a response (RFC 6886 section 3.5).
enum class ResultCode : uint16_t {
    Success = 0,
    UnsupportedVersion = 1,
    NotAuthorized = 2,
    NetworkFailure = 3,
    OutOfResources = 4,
    UnsupportedOpcode = 5,
};

/// The name of a result code, "Unknown" for a value outside the enumeration.
[[nodiscard]] const char* result_code_name(ResultCode code) noexcept;

/// A gateway's answer to the external-address request. Both fields are in
/// host byte order; `address` is the IPv4 address as a 32-bit integer.
struct ExternalAddress {
    uint32_t epoch_sec{0};  ///< Seconds since the gateway's mapping table started.
    uint32_t address{0};    ///< The gateway's public IPv4 address.
};

/// A UDP mapping the gateway granted or released. Fields in host byte order.
struct Mapping {
    uint32_t epoch_sec{0};      ///< Seconds since the gateway's mapping table started.
    uint16_t internal_port{0};  ///< The requested internal port, echoed.
    uint16_t external_port{0};  ///< The public port assigned; 0 after a release.
    uint32_t lifetime_sec{0};   ///< The granted lifetime; 0 after a release.
};

/// The 2-byte external-address request: version 0, opcode 0.
[[nodiscard]] std::vector<uint8_t> encode_external_address_request();

/// The 12-byte UDP mapping request (opcode 1): internal port, suggested
/// external port (0 for no preference), and the requested lifetime in
/// seconds. A lifetime of 0 with suggested port 0 releases the mapping.
[[nodiscard]] std::vector<uint8_t> encode_udp_map_request(uint16_t internal_port,
                                                          uint16_t suggested_external_port,
                                                          uint32_t lifetime_sec);

/// The result code of any response: nullopt when `bytes` is shorter than 4
/// or carries a version other than 0. The opcode is not checked, because a
/// gateway answers an unsupported version with opcode 0 (RFC 6886 section
/// 3.5).
[[nodiscard]] std::optional<ResultCode> decode_result_code(std::span<const uint8_t> bytes);

/// The parsed success response to the external-address request: nullopt
/// when the length is not 12, the version is not 0, the opcode is not 128,
/// or the result code is not Success.
[[nodiscard]] std::optional<ExternalAddress> decode_external_address(
    std::span<const uint8_t> bytes);

/// The parsed success response to a UDP mapping request: nullopt when the
/// length is not 16, the version is not 0, the opcode is not 129, or the
/// result code is not Success.
[[nodiscard]] std::optional<Mapping> decode_udp_mapping(std::span<const uint8_t> bytes);

/// Whether a gateway epoch shows that the gateway restarted since the last
/// response (RFC 6886 section 3.6). `previous_epoch` came with a response at
/// `previous_time_unix`; `current_epoch` came at `now_unix` (both Unix
/// seconds). The expected epoch is the previous one plus 7/8 of the elapsed
/// time; a current epoch more than 2 seconds below it means a restart, and
/// every mapping must be requested again. Time that moved backwards counts
/// as no elapsed time.
[[nodiscard]] bool epoch_indicates_restart(uint32_t previous_epoch, int64_t previous_time_unix,
                                           uint32_t current_epoch, int64_t now_unix);

/// Sends NAT-PMP requests to one gateway over UDP.
///
/// Every call is synchronous: it opens a UDP socket, sends the request,
/// waits for the answer with retransmissions at 250 ms, 500 ms, 1 s, and
/// 2 s (four sends, under 4 s in total; the RFC's 64 s budget would stall
/// the caller), and closes the socket. Only datagrams from the gateway
/// address and port are read; a datagram that does not decode is ignored
/// and the wait continues until the budget ends. No call throws.
///
/// Calls do not share a socket, but they write last_result_code() and
/// last_epoch_sec(), so serialize calls on one instance or use one instance
/// per thread.
class NatPmpClient {
public:
    /// `gateway_ipv4` is the gateway's dotted IPv4 address; `port` is its
    /// NAT-PMP port (kDefaultPort; a test passes its fake's loopback port).
    /// An address that does not parse makes every call fail.
    explicit NatPmpClient(std::string gateway_ipv4, uint16_t port = kDefaultPort);

    /// The gateway's public address. Fails when the gateway does not answer
    /// within the budget or answers with an error result code, which
    /// last_result_code() then names.
    [[nodiscard]] Result<ExternalAddress> external_address();

    /// Requests a public UDP port that forwards to `internal_port` (1 to
    /// 65535) for `lifetime_sec` seconds (1 or more; RFC 6886 recommends
    /// 7200). `suggested_external_port` is a preference the gateway may
    /// ignore; 0 means none. On success the returned Mapping holds the port
    /// and lifetime the gateway granted, which can differ from the request.
    /// Renew before the lifetime ends by calling again with the same
    /// internal port.
    [[nodiscard]] Result<Mapping> map_udp(uint16_t internal_port, uint16_t suggested_external_port,
                                          uint32_t lifetime_sec);

    /// Releases the UDP mapping of `internal_port` (1 to 65535): a mapping
    /// request with external port 0 and lifetime 0 (RFC 6886 section 3.4).
    /// Succeeds when the gateway confirms with result Success.
    [[nodiscard]] StatusResult release_udp(uint16_t internal_port);

    /// The result code of the last response the gateway sent, nullopt when
    /// no call has received a response yet.
    [[nodiscard]] std::optional<ResultCode> last_result_code() const noexcept;

    /// The epoch of the last response the gateway sent (0 before the first
    /// response). Compare with epoch_indicates_restart().
    [[nodiscard]] uint32_t last_epoch_sec() const noexcept;

private:
    /// Sends `request` until a datagram of `response_len` bytes arrives
    /// from the gateway with opcode `request_opcode` + kResponseBit, or an
    /// error result for that opcode. A mapping response must also echo
    /// `internal_port` when it is set. Returns the datagram on Success and
    /// records the result code and epoch of every accepted response.
    Result<std::vector<uint8_t>> exchange(const std::vector<uint8_t>& request,
                                          uint8_t request_opcode, size_t response_len,
                                          std::optional<uint16_t> internal_port);

    std::string gateway_;
    uint16_t port_;
    std::optional<ResultCode> last_result_code_;
    uint32_t last_epoch_sec_{0};
};

}  // namespace lnsdk::natpmp
