#include <LemonadeNexusSDK/PortMapper.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <utility>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

namespace lnsdk {

namespace {

constexpr int64_t kFirstBackoffSec = 30;
constexpr int64_t kMaxBackoffSec = 900;
constexpr int64_t kMaxRenewDelaySec = 1800;

bool parses_as_ipv4(const std::string& text) {
    in_addr addr{};
    return inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

std::string dotted(uint32_t host_order) {
    return std::to_string(host_order >> 24) + "." + std::to_string((host_order >> 16) & 0xFF) +
           "." + std::to_string((host_order >> 8) & 0xFF) + "." +
           std::to_string(host_order & 0xFF);
}

int64_t backoff_sec(uint32_t failures) {
    const unsigned shift = failures == 0 ? 0 : std::min(failures - 1, 5u);
    return std::min(kFirstBackoffSec << shift, kMaxBackoffSec);
}

bool publishable(AddressScope external, AddressScope server) {
    if (external == AddressScope::Public) return true;
    return external == AddressScope::Private &&
           (server == AddressScope::Private || server == AddressScope::Loopback);
}

}  // namespace

const char* mapping_protocol_name(MappingProtocol protocol) noexcept {
    switch (protocol) {
    case MappingProtocol::Pcp: return "pcp";
    case MappingProtocol::NatPmp: return "natpmp";
    case MappingProtocol::None: break;
    }
    return "none";
}

PortMapper::PortMapper(PortMapperOptions options) : options_(std::move(options)) {
    if (!options_.gateway_source) options_.gateway_source = default_gateway_ipv4;
    if (!options_.clock_unix) {
        options_.clock_unix = [] { return static_cast<int64_t>(std::time(nullptr)); };
    }
}

PortMapper::~PortMapper() {
    if (mapped()) release_on_gateway(state_.protocol, state_.internal_port);
}

bool PortMapper::maintain(uint16_t internal_port) {
    const std::string before = state_.external_endpoint;
    const int64_t at = now();
    if (internal_port == 0) {
        fail("internal port must not be 0", at);
        return false;
    }

    const auto route = options_.gateway_source();
    if (!route) {
        // The old gateway is gone with its mapping; a release could not reach it.
        if (mapped()) clear("the default gateway went away");
        fail("no default gateway", at);
        return before != state_.external_endpoint;
    }
    if (!parses_as_ipv4(route->gateway) || !parses_as_ipv4(route->local_address)) {
        if (mapped()) clear("the default gateway changed");
        fail("gateway route does not parse: " + route->gateway + " via " + route->local_address,
             at);
        return before != state_.external_endpoint;
    }

    if (mapped() && (route->gateway != state_.gateway ||
                     route->local_address != state_.local_address ||
                     internal_port != state_.internal_port)) {
        release_on_gateway(state_.protocol, state_.internal_port);
        clear(internal_port != state_.internal_port ? "the internal port changed"
                                                    : "the default gateway changed");
        failures_ = 0;
    }

    if (mapped()) {
        if (at >= state_.mapped_at_unix + static_cast<int64_t>(state_.lifetime_sec)) {
            clear("its lifetime ended");
        } else if (at < next_attempt_unix_) {
            return false;
        } else {
            std::string error;
            auto grant = renew(error);
            if (!grant) {
                fail(std::move(error), at);
                return false;
            }
            const bool restarted =
                grant->protocol == MappingProtocol::Pcp
                    ? pcp::epoch_indicates_restart(state_.epoch_sec, state_.mapped_at_unix,
                                                   grant->epoch_sec, at)
                    : natpmp::epoch_indicates_restart(state_.epoch_sec, state_.mapped_at_unix,
                                                      grant->epoch_sec, at);
            if (restarted) {
                spdlog::info("port mapping gateway {} restarted; port {} mapped again",
                             state_.gateway, state_.internal_port);
                if (grant->protocol == MappingProtocol::NatPmp) {
                    // A reboot can change the public address, and a NAT-PMP
                    // mapping response does not carry it.
                    auto address = natpmp_->external_address();
                    if (!address) {
                        fail(std::move(address.error), at);
                        return false;
                    }
                    grant->external_address = address.value.address;
                }
            }
            if (!accept(*grant, *route, internal_port, at, error)) fail(std::move(error), at);
            return before != state_.external_endpoint;
        }
    }

    if (at < next_attempt_unix_) return before != state_.external_endpoint;
    std::string error;
    auto grant = map(*route, internal_port, internal_port, error);
    if (!grant || !accept(*grant, *route, internal_port, at, error)) fail(std::move(error), at);
    return before != state_.external_endpoint;
}

int64_t PortMapper::seconds_until_due() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::max<int64_t>(0, next_attempt_unix_ - now());
}

void PortMapper::release() {
    if (mapped()) {
        release_on_gateway(state_.protocol, state_.internal_port);
        clear("released");
    }
    failures_ = 0;
    warned_error_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error.clear();
}

PortMapping PortMapper::mapping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::optional<PortMapper::Grant> PortMapper::map(const GatewayRoute& route,
                                                 uint16_t internal_port, uint16_t suggested,
                                                 std::string& error) {
    natpmp_.reset();
    pcp_.emplace(route.gateway, route.local_address, options_.gateway_port);
    auto r = pcp_->map_udp(internal_port, suggested, options_.lifetime_sec);
    if (r) {
        return Grant{MappingProtocol::Pcp, r.value.external_address, r.value.external_port,
                     r.value.lifetime_sec, r.value.epoch_sec};
    }
    // A PCP error result is the gateway's verdict; only silence or the
    // version fallback means it may speak NAT-PMP instead.
    if (!pcp_->unsupported_version() && pcp_->last_result_code()) {
        error = std::move(r.error);
        return std::nullopt;
    }
    pcp_.reset();
    return map_natpmp(route, internal_port, suggested, error);
}

std::optional<PortMapper::Grant> PortMapper::map_natpmp(const GatewayRoute& route,
                                                        uint16_t internal_port,
                                                        uint16_t suggested, std::string& error) {
    natpmp_.emplace(route.gateway, options_.gateway_port);
    auto address = natpmp_->external_address();
    if (!address) {
        error = std::move(address.error);
        return std::nullopt;
    }
    auto m = natpmp_->map_udp(internal_port, suggested, options_.lifetime_sec);
    if (!m) {
        error = std::move(m.error);
        return std::nullopt;
    }
    return Grant{MappingProtocol::NatPmp, address.value.address, m.value.external_port,
                 m.value.lifetime_sec, m.value.epoch_sec};
}

std::optional<PortMapper::Grant> PortMapper::renew(std::string& error) {
    if (state_.protocol == MappingProtocol::Pcp && pcp_) {
        auto r = pcp_->renew_udp(options_.lifetime_sec);
        if (!r) {
            error = std::move(r.error);
            return std::nullopt;
        }
        return Grant{MappingProtocol::Pcp, r.value.external_address, r.value.external_port,
                     r.value.lifetime_sec, r.value.epoch_sec};
    }
    if (state_.protocol == MappingProtocol::NatPmp && natpmp_) {
        auto m = natpmp_->map_udp(state_.internal_port, live_.external_port,
                                  options_.lifetime_sec);
        if (!m) {
            error = std::move(m.error);
            return std::nullopt;
        }
        return Grant{MappingProtocol::NatPmp, live_.external_address, m.value.external_port,
                     m.value.lifetime_sec, m.value.epoch_sec};
    }
    error = "no mapping to renew";
    return std::nullopt;
}

bool PortMapper::accept(const Grant& grant, const GatewayRoute& route, uint16_t internal_port,
                        int64_t at, std::string& error) {
    const std::string name = mapping_protocol_name(grant.protocol);
    if (grant.lifetime_sec == 0) {
        error = name + " gateway " + route.gateway + " granted no lifetime";
        pcp_.reset();
        natpmp_.reset();
        clear("the gateway granted no lifetime");
        return false;
    }
    if (grant.external_port == 0) {
        error = name + " gateway " + route.gateway + " granted external port 0";
        release_on_gateway(grant.protocol, internal_port);
        clear("the gateway granted external port 0");
        return false;
    }

    const std::string endpoint =
        dotted(grant.external_address) + ":" + std::to_string(grant.external_port);
    if (!publishable(endpoint_scope(endpoint), options_.server_scope)) {
        error = name + " gateway " + route.gateway + " assigned " + endpoint +
                ", which is not publishable toward the session server";
        release_on_gateway(grant.protocol, internal_port);
        clear("its external address is not publishable");
        return false;
    }

    PortMapping next;
    next.protocol = grant.protocol;
    next.gateway = route.gateway;
    next.local_address = route.local_address;
    next.external_endpoint = endpoint;
    next.internal_port = internal_port;
    next.lifetime_sec = grant.lifetime_sec;
    next.mapped_at_unix = at;
    next.renew_at_unix =
        at + std::min<int64_t>(std::max<int64_t>(1, grant.lifetime_sec / 2), kMaxRenewDelaySec);
    next.epoch_sec = grant.epoch_sec;

    if (next.protocol != state_.protocol || next.gateway != state_.gateway ||
        next.external_endpoint != state_.external_endpoint) {
        spdlog::info("port mapping via {}: {} for {} s through {}", name, endpoint,
                     grant.lifetime_sec, route.gateway);
    } else {
        spdlog::debug("port mapping via {} renewed: {} for {} s through {}", name, endpoint,
                      grant.lifetime_sec, route.gateway);
    }
    failures_ = 0;
    warned_error_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(next);
    live_ = grant;
    next_attempt_unix_ = state_.renew_at_unix;
    return true;
}

void PortMapper::release_on_gateway(MappingProtocol protocol, uint16_t internal_port) {
    StatusResult r;
    r.ok = true;
    if (protocol == MappingProtocol::Pcp && pcp_) {
        r = pcp_->release_udp(internal_port);
    } else if (protocol == MappingProtocol::NatPmp && natpmp_) {
        r = natpmp_->release_udp(internal_port);
    }
    if (!r) {
        spdlog::warn("port mapping release of port {} via {} failed: {}", internal_port,
                     mapping_protocol_name(protocol), r.error);
    }
    pcp_.reset();
    natpmp_.reset();
}

void PortMapper::clear(const char* reason) {
    if (!state_.external_endpoint.empty()) {
        spdlog::info("port mapping via {} ended ({}): {} through {}",
                     mapping_protocol_name(state_.protocol), reason, state_.external_endpoint,
                     state_.gateway);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string last_error = std::move(state_.last_error);
    state_ = PortMapping{};
    state_.last_error = std::move(last_error);
    live_ = Grant{};
    next_attempt_unix_ = 0;
}

void PortMapper::fail(std::string error, int64_t at) {
    ++failures_;
    if (error != warned_error_) {
        spdlog::warn("port mapping failed: {}", error);
        warned_error_ = error;
    }
    int64_t next = at + backoff_sec(failures_);
    if (mapped()) {
        next = std::min(next, state_.mapped_at_unix + static_cast<int64_t>(state_.lifetime_sec));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_.last_error = std::move(error);
    next_attempt_unix_ = next;
}

bool PortMapper::mapped() const { return state_.protocol != MappingProtocol::None; }

int64_t PortMapper::now() const { return options_.clock_unix(); }

}  // namespace lnsdk
