// Interface enumeration for LAN-direct paths on POSIX systems (Linux, macOS,
// iOS, and Android API 24 or later): getifaddrs.

#if !defined(_WIN32)

#include <LemonadeNexusSDK/LanPaths.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <bit>
#include <cstdint>

#if defined(__linux__)
#include <cstdio>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
// The media ioctl and its header exist in the macOS SDK only; iOS reports
// no link speed through this path.
#if TARGET_OS_OSX
#include <net/if_media.h>
#include <sys/ioctl.h>
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

}  // namespace lnsdk

#endif  // !_WIN32
