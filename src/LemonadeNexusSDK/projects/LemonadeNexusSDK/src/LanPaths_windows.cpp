// Interface enumeration for LAN-direct paths on Windows:
// GetAdaptersAddresses (iphlpapi).

#ifdef _WIN32

#include <LemonadeNexusSDK/LanPaths.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace lnsdk {

namespace {

std::string narrow(const wchar_t* text) {
    if (text == nullptr) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

/// Some VPN and hypervisor adapters report an Ethernet type; their
/// description names them.
bool virtual_adapter_description(std::string description) {
    static constexpr std::array<std::string_view, 7> kMarks = {
        "virtual", "vpn", "tap-", "hyper-v", "vmware", "virtualbox", "wireguard"};
    std::transform(description.begin(), description.end(), description.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(kMarks.begin(), kMarks.end(), [&](std::string_view mark) {
        return description.find(mark) != std::string::npos;
    });
}

}  // namespace

std::vector<InterfaceAddress> list_interface_addresses() {
    std::vector<InterfaceAddress> out;
    constexpr ULONG kFlags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    // The adapter list can grow between the two calls; retry a few times.
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        // Only wired and wireless LAN adapters carry LAN addresses. Tunnel,
        // PPP, and virtual adapter types (WireGuard and Wintun adapters
        // among them) are VPN or host-only links.
        const bool lan_type =
            (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211) &&
            !virtual_adapter_description(narrow(adapter->Description));
        const auto name = narrow(adapter->FriendlyName);
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const auto* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr || sa->sa_family != AF_INET) continue;
            const auto* addr = reinterpret_cast<const sockaddr_in*>(sa);
            char text[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof text) == nullptr) continue;
            InterfaceAddress itf;
            itf.name = name;
            itf.address = text;
            itf.prefix_len = static_cast<uint8_t>(unicast->OnLinkPrefixLength);
            itf.up = adapter->OperStatus == IfOperStatusUp;
            itf.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            itf.tunnel = !lan_type && !itf.loopback;
            out.push_back(std::move(itf));
        }
    }
    return out;
}

}  // namespace lnsdk

#endif  // _WIN32
