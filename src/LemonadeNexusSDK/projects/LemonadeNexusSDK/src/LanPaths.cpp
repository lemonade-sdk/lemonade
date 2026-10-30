#include <LemonadeNexusSDK/LanPaths.hpp>

#include <LemonadeNexusSDK/HolePunch.hpp>

#include <LemonadeNexus/Boringtun/IpRouter.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>

namespace lnsdk {

using nexus::boringtun::Cidr;

namespace {

/// A dotted IPv4 address (no prefix, no port) in host byte order.
std::optional<uint32_t> parse_address(const std::string& text) {
    if (text.empty() || text.find('/') != std::string::npos) return std::nullopt;
    const auto cidr = Cidr::parse(text);
    if (!cidr || cidr->prefix_len != 32) return std::nullopt;
    return cidr->network;
}

bool in_block(uint32_t ip, uint32_t network, int prefix) {
    const uint32_t mask = prefix == 0 ? 0 : ~uint32_t{0} << (32 - prefix);
    return (ip & mask) == network;
}

bool shared_address_space(uint32_t ip) { return in_block(ip, 0x64400000, 10); }

/// The address part of "a.b.c.d:port".
std::string host_of(const std::string& endpoint) {
    const auto colon = endpoint.find(':');
    return colon == std::string::npos ? endpoint : endpoint.substr(0, colon);
}

}  // namespace

bool virtual_interface_name(std::string_view name) {
    static constexpr std::array<std::string_view, 21> kPrefixes = {
        "utun", "tun",  "tap",  "wg",   "ppp",     "ipsec",  "gif",
        "stf",  "awdl", "llw",  "docker", "veth",  "br-",    "virbr",
        "vmnet", "vboxnet", "zt", "tailscale", "rmnet", "ccmni", "pdp_ip",
    };
    std::string lower;
    lower.reserve(name.size());
    for (const char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower.starts_with("clat") || lower.starts_with("v4-")) return true;
    return std::any_of(kPrefixes.begin(), kPrefixes.end(),
                       [&lower](std::string_view prefix) { return lower.starts_with(prefix); });
}

std::vector<InterfaceAddress> usable_lan_interfaces(const std::vector<InterfaceAddress>& interfaces,
                                                    const LocalEndpointRules& rules) {
    std::vector<Cidr> mesh;
    for (const auto& text : rules.mesh_ranges) {
        if (auto cidr = Cidr::parse(text)) mesh.push_back(*cidr);
    }
    const auto reflexive = parse_address(host_of(rules.reflexive));

    std::vector<InterfaceAddress> out;
    for (const auto& itf : interfaces) {
        if (out.size() >= kMaxLocalEndpoints) break;
        if (!itf.up || itf.tunnel || virtual_interface_name(itf.name)) continue;
        if (itf.loopback && !rules.allow_loopback) continue;
        const auto ip = parse_address(itf.address);
        if (!ip) continue;
        if (std::any_of(mesh.begin(), mesh.end(),
                        [&](const Cidr& c) { return c.contains(*ip); })) {
            continue;
        }
        bool usable = false;
        switch (endpoint_scope(itf.address + ":1")) {
            case AddressScope::Private:
                // A shared address space address is a carrier's, behind its
                // NAT, unless STUN saw this very address.
                usable = !shared_address_space(*ip) || (reflexive && *reflexive == *ip);
                break;
            case AddressScope::Loopback:
                usable = rules.allow_loopback;
                break;
            case AddressScope::Public:
            case AddressScope::Invalid:
                break;
        }
        if (!usable) continue;
        if (std::any_of(out.begin(), out.end(),
                        [&](const InterfaceAddress& o) { return o.address == itf.address; })) {
            continue;
        }
        out.push_back(itf);
    }
    return out;
}

std::vector<std::string> select_local_endpoints(const std::vector<InterfaceAddress>& interfaces,
                                                uint16_t port, const LocalEndpointRules& rules) {
    std::vector<std::string> out;
    if (port == 0) return out;
    for (const auto& itf : usable_lan_interfaces(interfaces, rules)) {
        out.push_back(itf.address + ":" + std::to_string(port));
    }
    return out;
}

bool lan_endpoint_usable(const std::string& endpoint, bool server_on_loopback) {
    switch (endpoint_scope(endpoint)) {
        case AddressScope::Private:
            return true;
        case AddressScope::Loopback:
            return server_on_loopback;
        case AddressScope::Public:
        case AddressScope::Invalid:
            break;
    }
    return false;
}

std::vector<std::string> lan_probe_targets(const MeshPeer& peer, const std::string& own_reflexive,
                                           const std::vector<InterfaceAddress>& own_lan) {
    if (peer.local_endpoints.empty()) return {};
    const auto own_public = parse_address(host_of(own_reflexive));
    const auto peer_public = parse_address(host_of(peer.endpoint));
    if (own_public && peer_public && *own_public == *peer_public) return peer.local_endpoints;

    for (const auto& endpoint : peer.local_endpoints) {
        const auto ip = parse_address(host_of(endpoint));
        if (!ip) continue;
        for (const auto& itf : own_lan) {
            const auto own = parse_address(itf.address);
            if (!own) continue;
            const int prefix = std::min<int>(itf.prefix_len, 32);
            const uint32_t mask = prefix == 0 ? 0 : ~uint32_t{0} << (32 - prefix);
            if ((*ip & mask) == (*own & mask)) return peer.local_endpoints;
        }
    }
    return {};
}

}  // namespace lnsdk
