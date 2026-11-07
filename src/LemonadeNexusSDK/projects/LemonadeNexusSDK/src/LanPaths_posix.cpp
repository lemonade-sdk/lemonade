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

namespace lnsdk {

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
        out.push_back(std::move(itf));
    }
    freeifaddrs(list);
    return out;
}

}  // namespace lnsdk

#endif  // !_WIN32
