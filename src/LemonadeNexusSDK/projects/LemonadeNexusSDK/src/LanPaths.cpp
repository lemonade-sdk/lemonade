#include <LemonadeNexusSDK/LanPaths.hpp>

#include <LemonadeNexusSDK/HolePunch.hpp>

#include <LemonadeNexus/Boringtun/IpRouter.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

namespace {

/// The network address of `ip` under `prefix` (0 to 32), host byte order.
uint32_t network_of(uint32_t ip, uint8_t prefix) {
    const int p = std::min<int>(prefix, 32);
    const uint32_t mask = p == 0 ? 0 : ~uint32_t{0} << (32 - p);
    return ip & mask;
}

}  // namespace

std::vector<InterfaceAddress> usable_lan_interfaces(const std::vector<InterfaceAddress>& interfaces,
                                                    const LocalEndpointRules& rules) {
    std::vector<Cidr> mesh;
    for (const auto& text : rules.mesh_ranges) {
        if (auto cidr = Cidr::parse(text)) mesh.push_back(*cidr);
    }
    const auto reflexive = parse_address(host_of(rules.reflexive));

    // The usable interfaces, each paired with its parsed address, in list
    // order. The cap and the subnet dedup come after the speed sort, so the
    // fastest address of a subnet is the one that survives.
    std::vector<std::pair<InterfaceAddress, uint32_t>> usable;
    for (const auto& itf : interfaces) {
        if (!itf.up || itf.tunnel || virtual_interface_name(itf.name)) continue;
        if (itf.loopback && !rules.allow_loopback) continue;
        const auto ip = parse_address(itf.address);
        if (!ip) continue;
        if (std::any_of(mesh.begin(), mesh.end(),
                        [&](const Cidr& c) { return c.contains(*ip); })) {
            continue;
        }
        bool ok = false;
        switch (endpoint_scope(itf.address + ":1")) {
            case AddressScope::Private:
                // A shared address space address is a carrier's, behind its
                // NAT, unless STUN saw this very address.
                ok = !shared_address_space(*ip) || (reflexive && *reflexive == *ip);
                break;
            case AddressScope::Loopback:
                ok = rules.allow_loopback;
                break;
            case AddressScope::Public:
            case AddressScope::Invalid:
                break;
        }
        if (!ok) continue;
        if (std::any_of(usable.begin(), usable.end(),
                        [&](const auto& o) { return o.first.address == itf.address; })) {
            continue;
        }
        usable.emplace_back(itf, *ip);
    }

    // Fastest link first. A stable sort keeps the platform's list order among
    // interfaces that report the same speed (0 when the speed is unknown).
    std::stable_sort(usable.begin(), usable.end(), [](const auto& a, const auto& b) {
        return a.first.link_speed_mbps > b.first.link_speed_mbps;
    });

    std::vector<InterfaceAddress> out;
    std::vector<std::pair<uint32_t, uint8_t>> subnets;  // network, prefix already taken
    for (const auto& [itf, ip] : usable) {
        if (out.size() >= kMaxLocalEndpoints) break;
        const uint32_t net = network_of(ip, itf.prefix_len);
        // Skip a slower address on a subnet the faster address already covers
        // (the sort put the faster one first).
        if (std::any_of(subnets.begin(), subnets.end(), [&](const auto& s) {
                return s.second == itf.prefix_len && s.first == net;
            })) {
            continue;
        }
        subnets.emplace_back(net, itf.prefix_len);
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

bool gateway_address_usable(const std::string& gateway) {
    if (endpoint_scope(gateway + ":1") == AddressScope::Private) return true;
    const auto ip = parse_address(gateway);
    return ip && in_block(*ip, 0xA9FE0000, 16);
}

namespace {

/// The next whitespace-delimited field of `line`, consumed from its front.
std::string_view next_field(std::string_view& line) {
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(start);
    const auto end = line.find_first_of(" \t");
    const auto field = line.substr(0, end);
    line.remove_prefix(end == std::string_view::npos ? line.size() : end);
    return field;
}

template <typename T>
std::optional<T> parse_number(std::string_view field, int base) {
    T value{};
    const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value, base);
    if (ec != std::errc{} || ptr != field.data() + field.size()) return std::nullopt;
    return value;
}

/// The dotted form of a /proc/net/route address: 8 hex digits of the
/// address in little-endian byte order ("0101A8C0" is 192.168.1.1).
std::optional<std::string> dotted_from_route_hex(std::string_view field) {
    if (field.size() != 8) return std::nullopt;
    const auto value = parse_number<uint32_t>(field, 16);
    if (!value) return std::nullopt;
    return std::to_string(*value & 0xff) + "." + std::to_string((*value >> 8) & 0xff) + "." +
           std::to_string((*value >> 16) & 0xff) + "." + std::to_string((*value >> 24) & 0xff);
}

}  // namespace

std::optional<GatewayRoute> parse_proc_net_route(std::string_view text,
                                                 const std::vector<InterfaceAddress>& interfaces) {
    constexpr unsigned kRtfUp = 0x0001;
    constexpr unsigned kRtfGateway = 0x0002;
    std::optional<GatewayRoute> best;
    unsigned long best_metric = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const auto iface = next_field(line);
        if (iface.empty() || iface == "Iface") continue;
        const auto destination = next_field(line);
        const auto gateway_hex = next_field(line);
        const auto flags = parse_number<unsigned>(next_field(line), 16);
        next_field(line);  // RefCnt
        next_field(line);  // Use
        const auto metric = parse_number<unsigned long>(next_field(line), 10);
        if (destination != "00000000" || !flags || !metric) continue;
        if ((*flags & (kRtfUp | kRtfGateway)) != (kRtfUp | kRtfGateway)) continue;
        const auto gateway = dotted_from_route_hex(gateway_hex);
        if (!gateway || *gateway == "0.0.0.0") continue;
        if (best && *metric >= best_metric) continue;

        GatewayRoute route;
        route.gateway = *gateway;
        route.interface_name = std::string(iface);
        const auto itf = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto& a) {
            return a.name == iface;
        });
        if (itf != interfaces.end()) route.local_address = itf->address;
        best = std::move(route);
        best_metric = *metric;
    }
    return best;
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
