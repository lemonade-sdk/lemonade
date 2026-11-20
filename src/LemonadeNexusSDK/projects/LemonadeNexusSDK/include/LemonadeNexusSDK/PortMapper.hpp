#pragma once

/// Keeps one UDP port mapping alive on the default gateway. The mapper
/// finds the gateway (LanPaths.hpp), asks it with PCP (Pcp.hpp) and falls
/// back to NAT-PMP (NatPmp.hpp) when the gateway does not speak PCP, checks
/// the answer, renews the mapping before it ends, and maps again after a
/// network change or a gateway restart. The orchestrator publishes the
/// external endpoint the mapper reports.
///
/// A gateway's answer is untrusted input: an external address is published
/// only when its scope fits the session server's scope, and a mapping with
/// lifetime 0 or external port 0 counts as no mapping.

#include <LemonadeNexusSDK/HolePunch.hpp>
#include <LemonadeNexusSDK/LanPaths.hpp>
#include <LemonadeNexusSDK/NatPmp.hpp>
#include <LemonadeNexusSDK/Pcp.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace lnsdk {

/// The protocol a mapping was obtained with.
enum class MappingProtocol : uint8_t { None, Pcp, NatPmp };

/// "none", "pcp", or "natpmp".
[[nodiscard]] const char* mapping_protocol_name(MappingProtocol protocol) noexcept;

/// The state of the mapping, as PortMapper::mapping() reports it. A
/// protocol of None means nothing is mapped; `last_error` then says why the
/// last attempt failed (empty before the first attempt and after a
/// release).
struct PortMapping {
    MappingProtocol protocol{MappingProtocol::None};
    std::string gateway;            ///< Dotted IPv4 address the mapping was obtained from.
    std::string local_address;      ///< Our address on the route to the gateway.
    std::string external_endpoint;  ///< "a.b.c.d:port" to publish; empty when none.
    uint16_t internal_port{0};      ///< The port on this device the mapping forwards to.
    uint32_t lifetime_sec{0};       ///< The lifetime the gateway granted.
    int64_t mapped_at_unix{0};      ///< Unix time of the gateway's last grant.
    int64_t renew_at_unix{0};       ///< mapped_at plus half the lifetime, at most 30 min later.
    uint32_t epoch_sec{0};          ///< The gateway epoch that came with the grant.
    std::string last_error;         ///< The last failure; empty after a success.
};

/// What a PortMapper needs besides the internal port.
struct PortMapperOptions {
    /// The lifetime to request, in seconds (1 or more). The gateway may
    /// grant less.
    uint32_t lifetime_sec{7200};
    /// The gateway's PCP and NAT-PMP port. A test points it at its fake.
    uint16_t gateway_port{5351};
    /// The scope of the session server's mesh endpoint. A private external
    /// address is accepted only when the server is private or loopback.
    AddressScope server_scope{AddressScope::Public};
    /// Where the default gateway comes from; default_gateway_ipv4() when
    /// unset. Nullopt means no gateway.
    std::function<std::optional<GatewayRoute>()> gateway_source;
    /// Unix time in seconds; std::time when unset.
    std::function<int64_t()> clock_unix;
};

/// Obtains and maintains the mapping for one internal UDP port.
///
/// maintain() and release() must be called from one thread (the
/// orchestrator's mapping thread); they make synchronous, bounded gateway
/// exchanges. cancel() ends a maintain() exchange early from another
/// thread. mapping() and seconds_until_due() may be called from any thread
/// and read the state under a mutex. No method throws.
class PortMapper {
public:
    explicit PortMapper(PortMapperOptions options = {});

    /// Releases a live mapping on the gateway (bounded by the client's
    /// release budget).
    ~PortMapper();

    PortMapper(const PortMapper&) = delete;
    PortMapper& operator=(const PortMapper&) = delete;

    /// Obtains or refreshes the mapping for `internal_port` (1 to 65535).
    /// Finds the gateway; when the gateway or the port differs from the
    /// live mapping, releases that mapping (best effort) and maps again.
    /// A first attempt tries PCP, then NAT-PMP when the gateway answered
    /// the version fallback or did not answer; a PCP error result ends the
    /// attempt, because the gateway speaks PCP and refused. A renewal is
    /// sent when its time has come; a renewal that fails keeps the mapping
    /// published until its lifetime ends. A gateway restart that the epoch
    /// shows counts as a new mapping. After a failure the next attempt
    /// waits 30 s, 60 s, 120 s, and so on up to 15 min; a call before that
    /// time does nothing. Synchronous and bounded by the clients' budgets:
    /// about 11 s for a first attempt that no gateway answers, 2.5 s for
    /// a PCP renewal, under 8 s for a NAT-PMP renewal. Returns true when
    /// the published endpoint changed.
    bool maintain(uint16_t internal_port);

    /// Seconds until maintain() has work: the next renewal, or the end of
    /// the retry backoff; 0 when it is due now or nothing has been tried.
    [[nodiscard]] int64_t seconds_until_due() const;

    /// Ends the gateway exchange of a maintain() call in progress within
    /// about a quarter second, and makes a maintain() call before the next
    /// release() return at once. The attempt it ends is not counted as a
    /// failure and sets no retry. Thread-safe.
    void cancel();

    /// Deletes the live mapping on the gateway with a lifetime-0 request of
    /// its protocol and clears the state. Does nothing when nothing is
    /// mapped. Runs after a cancel() and ends its effect.
    void release();

    /// A copy of the current state. Thread-safe.
    [[nodiscard]] PortMapping mapping() const;

private:
    /// What one gateway exchange established, before the scope check.
    struct Grant {
        MappingProtocol protocol{MappingProtocol::None};
        uint32_t external_address{0};  ///< Host byte order.
        uint16_t external_port{0};
        uint32_t lifetime_sec{0};
        uint32_t epoch_sec{0};
    };

    /// Maps `internal_port` through `route` with fresh clients: PCP first,
    /// then NAT-PMP when PCP gave no verdict. `suggested` is the external
    /// port to ask for. Fails with the reason in `error`.
    std::optional<Grant> map(const GatewayRoute& route, uint16_t internal_port,
                             uint16_t suggested, std::string& error);
    std::optional<Grant> map_natpmp(const GatewayRoute& route, uint16_t internal_port,
                                    uint16_t suggested, std::string& error);
    /// Renews the live mapping with its protocol. Fails with the reason in
    /// `error`.
    std::optional<Grant> renew(std::string& error);
    /// Checks `grant` for `internal_port` through `route` and makes it the
    /// published mapping at time `at`; takes it back from the gateway when
    /// its external address may not be published. False with the reason
    /// in `error`.
    bool accept(const Grant& grant, const GatewayRoute& route, uint16_t internal_port,
                int64_t at, std::string& error);
    /// Sends the lifetime-0 request for `internal_port` with the client of
    /// `protocol`, logs a failure, and drops both clients.
    void release_on_gateway(MappingProtocol protocol, uint16_t internal_port);
    /// Forgets the published mapping (keeps `last_error`) and makes the
    /// next attempt due now; logs `reason` when something was published.
    void clear(const char* reason);
    /// Records a failed attempt: sets `last_error`, warns once per cause,
    /// and schedules the retry.
    void fail(std::string error, int64_t at);
    bool mapped() const;
    int64_t now() const;

    PortMapperOptions options_;
    mutable std::mutex mutex_;
    PortMapping state_;
    Grant live_;  ///< The grant behind `state_`.
    std::optional<pcp::PcpClient> pcp_;
    std::optional<natpmp::NatPmpClient> natpmp_;
    std::atomic<bool> cancel_{false};  ///< Read by the clients' exchanges.
    int64_t next_attempt_unix_{0};
    uint32_t failures_{0};
    std::string warned_error_;
};

}  // namespace lnsdk
