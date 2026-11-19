// Interface enumeration and default-gateway lookup for LAN-direct paths on
// POSIX systems (Linux, macOS, iOS, and Android API 24 or later): getifaddrs,
// /proc/net/route on Linux, and the PF_ROUTE sysctl on Apple platforms.

#if !defined(_WIN32)

#include <LemonadeNexusSDK/LanPaths.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <cstdio>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
// The media ioctl, its header and net/route.h exist in the macOS SDK only;
// iOS reports no link speed and no default gateway through these paths.
#if TARGET_OS_OSX
#include <net/if_media.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif
#endif

namespace lnsdk {

namespace {

/// The link speed of interface `name` in Mbit/s, or 0 when the platform does
/// not report one (Wi-Fi, a down link, or an unsupported build).
uint32_t interface_link_speed(const char* name) {
    if (name == nullptr || name[0] == '\0') return 0;
#if defined(__linux__)
    // /sys/class/net/<name>/speed reports Mbit/s; it is -1 or unreadable for
    // a Wi-Fi or down interface, which reads as 0.
    char path[64];
    if (std::snprintf(path, sizeof path, "/sys/class/net/%s/speed", name) <= 0) return 0;
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return 0;
    long value = -1;
    const int read = std::fscanf(f, "%ld", &value);
    std::fclose(f);
    if (read != 1 || value < 0 || value > 0x7fffffff) return 0;
    return static_cast<uint32_t>(value);
#elif defined(__APPLE__) && TARGET_OS_OSX
    // The active media word carries the Ethernet subtype. Wi-Fi reports
    // IFM_IEEE80211 with no rate here, so it maps to 0.
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    ifmediareq req{};
    std::snprintf(req.ifm_name, sizeof req.ifm_name, "%s", name);
    uint32_t mbps = 0;
    if (ioctl(s, SIOCGIFMEDIA, &req) == 0 && (req.ifm_status & IFM_AVALID) != 0 &&
        IFM_TYPE(req.ifm_active) == IFM_ETHER) {
        switch (IFM_SUBTYPE(req.ifm_active)) {
            case IFM_10_T:
            case IFM_10_2:
            case IFM_10_5:
            case IFM_10_FL:
            case IFM_10_STP:
                mbps = 10;
                break;
            case IFM_100_TX:
            case IFM_100_FX:
            case IFM_100_T4:
            case IFM_100_VG:
            case IFM_100_T2:
                mbps = 100;
                break;
            case IFM_1000_SX:
            case IFM_1000_LX:
            case IFM_1000_CX:
            case IFM_1000_T:
                mbps = 1000;
                break;
#ifdef IFM_2500_T
            case IFM_2500_T:
                mbps = 2500;
                break;
#endif
#ifdef IFM_5000_T
            case IFM_5000_T:
                mbps = 5000;
                break;
#endif
#ifdef IFM_10G_T
            case IFM_10G_T:
#endif
#ifdef IFM_10G_SR
            case IFM_10G_SR:
#endif
#ifdef IFM_10G_LR
            case IFM_10G_LR:
#endif
                mbps = 10000;
                break;
#ifdef IFM_25G_T
            case IFM_25G_T:
                mbps = 25000;
                break;
#endif
#ifdef IFM_40G_CR4
            case IFM_40G_CR4:
                mbps = 40000;
                break;
#endif
            default:
                mbps = 0;
                break;
        }
    }
    close(s);
    return mbps;
#else
    // iOS and Android report no portable link speed here.
    (void)name;
    return 0;
#endif
}

}  // namespace

std::vector<InterfaceAddress> list_interface_addresses() {
    std::vector<InterfaceAddress> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (const ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) continue;
        const auto* addr = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof text) == nullptr) continue;
        InterfaceAddress itf;
        itf.name = ifa->ifa_name != nullptr ? ifa->ifa_name : "";
        itf.address = text;
        if (ifa->ifa_netmask != nullptr && ifa->ifa_netmask->sa_family == AF_INET) {
            const auto* mask = reinterpret_cast<const sockaddr_in*>(ifa->ifa_netmask);
            itf.prefix_len = static_cast<uint8_t>(std::popcount(ntohl(mask->sin_addr.s_addr)));
        }
        const auto flags = ifa->ifa_flags;
        itf.up = (flags & IFF_UP) != 0 && (flags & IFF_RUNNING) != 0;
        itf.loopback = (flags & IFF_LOOPBACK) != 0;
        itf.tunnel = (flags & IFF_POINTOPOINT) != 0;
        itf.link_speed_mbps = interface_link_speed(itf.name.c_str());
        out.push_back(std::move(itf));
    }
    freeifaddrs(list);
    return out;
}

namespace {

#if defined(__linux__)

std::optional<GatewayRoute> platform_default_gateway() {
    // Android refuses /proc/net/route to apps on recent releases; the open
    // fails and the lookup reports no gateway.
    std::FILE* f = std::fopen("/proc/net/route", "re");
    if (f == nullptr) return std::nullopt;
    std::string text;
    char chunk[4096];
    for (;;) {
        const auto n = std::fread(chunk, 1, sizeof chunk, f);
        text.append(chunk, n);
        if (n < sizeof chunk) break;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) return std::nullopt;
    return parse_proc_net_route(text, list_interface_addresses());
}

#elif defined(__APPLE__) && TARGET_OS_OSX

/// The IPv4 address of the up interface named `name`, or empty.
std::string address_of_interface(const std::vector<InterfaceAddress>& interfaces,
                                 const std::string& name) {
    const auto itf = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto& a) {
        return a.up && a.name == name;
    });
    return itf == interfaces.end() ? std::string{} : itf->address;
}

/// Socket addresses in a routing message are aligned to 4 bytes; a zero
/// sa_len still takes one slot (route(8) and netstat use this rule).
std::size_t route_sockaddr_size(uint8_t sa_len) {
    return sa_len > 0 ? 1 + ((sa_len - 1) | (sizeof(uint32_t) - 1)) : sizeof(uint32_t);
}

bool zero_ipv4(const sockaddr* sa) {
    if (sa == nullptr || sa->sa_len == 0) return true;
    if (sa->sa_family != AF_INET || sa->sa_len < sizeof(sockaddr_in)) return false;
    sockaddr_in in{};
    std::memcpy(&in, sa, sizeof in);
    return in.sin_addr.s_addr == 0;
}

std::optional<GatewayRoute> platform_default_gateway() {
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    std::size_t len = 0;
    if (sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0) return std::nullopt;
    // The table can grow between the size query and the read.
    std::vector<char> buffer(len + 1024);
    len = buffer.size();
    if (sysctl(mib, 6, buffer.data(), &len, nullptr, 0) != 0) return std::nullopt;

    const auto interfaces = list_interface_addresses();
    std::optional<GatewayRoute> scoped;
    const char* const end = buffer.data() + len;
    for (const char* p = buffer.data(); p + sizeof(rt_msghdr) <= end;) {
        rt_msghdr rtm{};
        std::memcpy(&rtm, p, sizeof rtm);
        if (rtm.rtm_msglen < sizeof(rt_msghdr) || p + rtm.rtm_msglen > end) break;
        const char* const record_end = p + rtm.rtm_msglen;
        const char* const record = p;
        p = record_end;
        if (rtm.rtm_version != RTM_VERSION) continue;
        if ((rtm.rtm_flags & (RTF_UP | RTF_GATEWAY)) != (RTF_UP | RTF_GATEWAY)) continue;
        if ((rtm.rtm_flags & RTF_HOST) != 0) continue;

        const sockaddr* addrs[RTAX_MAX] = {};
        const char* cursor = record + sizeof(rt_msghdr);
        bool truncated = false;
        for (int i = 0; i < RTAX_MAX; ++i) {
            if ((rtm.rtm_addrs & (1 << i)) == 0) continue;
            if (cursor + 1 > record_end) {
                truncated = true;
                break;
            }
            const auto* sa = reinterpret_cast<const sockaddr*>(cursor);
            const auto size = route_sockaddr_size(sa->sa_len);
            if (cursor + size > record_end) {
                truncated = true;
                break;
            }
            addrs[i] = sa;
            cursor += size;
        }
        if (truncated) continue;
        // The default route has destination 0.0.0.0 and no mask (or a zero mask).
        if ((rtm.rtm_addrs & RTA_DST) == 0 || !zero_ipv4(addrs[RTAX_DST])) continue;
        if ((rtm.rtm_addrs & RTA_NETMASK) != 0 && !zero_ipv4(addrs[RTAX_NETMASK])) continue;
        const sockaddr* gw = addrs[RTAX_GATEWAY];
        if (gw == nullptr || gw->sa_family != AF_INET || gw->sa_len < sizeof(sockaddr_in)) continue;
        sockaddr_in gw_in{};
        std::memcpy(&gw_in, gw, sizeof gw_in);
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &gw_in.sin_addr, text, sizeof text) == nullptr) continue;

        GatewayRoute route;
        route.gateway = text;
        char name[IF_NAMESIZE] = {};
        if (if_indextoname(rtm.rtm_index, name) != nullptr) route.interface_name = name;
        route.local_address = address_of_interface(interfaces, route.interface_name);
#ifdef RTF_IFSCOPE
        // An interface-scoped default route is a secondary path; the
        // unscoped one is the route the system uses.
        if ((rtm.rtm_flags & RTF_IFSCOPE) != 0) {
            if (!scoped) scoped = std::move(route);
            continue;
        }
#endif
        return route;
    }
    return scoped;
}

#else

std::optional<GatewayRoute> platform_default_gateway() { return std::nullopt; }

#endif

}  // namespace

std::optional<GatewayRoute> default_gateway_ipv4() {
    auto route = platform_default_gateway();
    if (!route || route->local_address.empty() || !gateway_address_usable(route->gateway)) {
        return std::nullopt;
    }
    return route;
}

}  // namespace lnsdk

#endif  // !_WIN32
