#pragma once

/// One request-response exchange with a gateway over UDP, shared by the
/// NAT-PMP and PCP clients. Internal to the SDK.

#include <LemonadeNexusSDK/Error.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lnsdk::udp_exchange {

/// Decides whether a datagram from the gateway ends the exchange. It sees
/// every datagram the gateway sent to the socket; it returns true for the
/// one the caller wants, which exchange() then returns.
using Acceptor = std::function<bool(std::span<const uint8_t>)>;

/// Sends `request` to `gateway_ipv4`:`port` from a fresh UDP socket and
/// reads replies until `accept` takes one or the schedule ends. `waits` is
/// the time to wait after each send; the request is sent once per entry, so
/// the whole exchange lasts at most the sum of `waits`. Only datagrams from
/// the gateway address and port reach `accept`; a datagram it refuses is
/// dropped and the wait continues. When `local_ipv4` is set, the socket is
/// bound to that address so the gateway sees it as the source.
///
/// Returns the accepted datagram, or an error that starts with `protocol`
/// (for example "PCP gateway did not answer") when the address does not
/// parse, the socket or send fails, or the schedule ends without an
/// accepted reply. The socket is closed before returning. Never throws.
///
/// `cancel`, when set, is read every kCancelPoll at most: once it holds
/// true the exchange ends within that time with the error
/// "<protocol> exchange cancelled". Another thread may set it.
[[nodiscard]] Result<std::vector<uint8_t>> exchange(
    const std::string& gateway_ipv4, uint16_t port, const std::optional<std::string>& local_ipv4,
    std::span<const uint8_t> request, std::span<const std::chrono::milliseconds> waits,
    const Acceptor& accept, const char* protocol, const std::atomic<bool>* cancel = nullptr);

/// The longest wait between two reads of `cancel`.
inline constexpr std::chrono::milliseconds kCancelPoll{250};

}  // namespace lnsdk::udp_exchange
