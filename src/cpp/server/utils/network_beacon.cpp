#include "lemon/utils/network_beacon.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    #define INVALID_SOCKET_NB INVALID_SOCKET
#else
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>

    #define closesocket close
    #define INVALID_SOCKET_NB -1
#endif

NetworkBeacon::NetworkBeacon() : _socket(INVALID_SOCKET_NB), _isInitialized(false), _netThreadRunning(false) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
    _isInitialized = true;
}

NetworkBeacon::~NetworkBeacon() {
    stopBroadcasting();
    cleanup();
}

void NetworkBeacon::cleanup() {
    if (_socket != INVALID_SOCKET_NB) {
        closesocket(_socket);
        _socket = INVALID_SOCKET_NB;
    }
#ifdef _WIN32
    if (_isInitialized) {
        WSACleanup();
        _isInitialized = false;
    }
#endif
}

void NetworkBeacon::createSocket() {
    if (_socket != INVALID_SOCKET_NB) {
        closesocket(_socket);
    }
    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_socket == INVALID_SOCKET_NB) {
        throw std::runtime_error("Could not create socket");
    }
}

std::string NetworkBeacon::getLocalHostname() {
    char buffer[256];
    if (gethostname(buffer, sizeof(buffer)) == 0) {
        return std::string(buffer);
    }
    return "UnknownHost";
}

bool NetworkBeacon::isRFC1918(const std::string& ipAddress) {
    struct in_addr addr;

    // Convert string to network address structure
    if (inet_pton(AF_INET, ipAddress.c_str(), &addr) != 1) {
        return false; // Not a valid IPv4 address
    }

    // Convert to host byte order for easier comparison
    uint32_t ip = ntohl(addr.s_addr);

    // 10.0.0.0/8
    if ((ip & 0xFF000000) == 0x0A000000) return true;

    // 172.16.0.0/12 (172.16.0.0 - 172.31.255.255)
    if ((ip & 0xFFF00000) == 0xAC100000) return true;

    // 192.168.0.0/16
    if ((ip & 0xFFFF0000) == 0xC0A80000) return true;

    // 127.0.0.0/8 (loopback)
    if ((ip & 0xFF000000) == 0x7F000000) return true;

    return false;
}

std::vector<NetworkInterfaceInfo> NetworkBeacon::getLocalRFC1918Interfaces() {
    std::vector<NetworkInterfaceInfo> interfaces;

#ifdef _WIN32
    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES adapters = nullptr;
    ULONG ret = 0;

    do {
        adapters = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
        if (!adapters) return interfaces;
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            free(adapters);
            adapters = nullptr;
        }
    } while (ret == ERROR_BUFFER_OVERFLOW);

    if (ret != NO_ERROR) {
        if (adapters) free(adapters);
        return interfaces;
    }

    for (auto adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;

        for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;

            auto sa = (struct sockaddr_in*)unicast->Address.lpSockaddr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

            if (!isRFC1918(ip)) continue;

            // Compute broadcast from IP and prefix length
            uint32_t ipAddr = ntohl(sa->sin_addr.s_addr);
            uint32_t mask = (unicast->OnLinkPrefixLength == 0) ? 0 : (~0U << (32 - unicast->OnLinkPrefixLength));
            uint32_t bcast = ipAddr | ~mask;

            struct in_addr bcastAddr;
            bcastAddr.s_addr = htonl(bcast);
            char bcastStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &bcastAddr, bcastStr, sizeof(bcastStr));

            interfaces.push_back({ip, bcastStr});
        }
    }

    free(adapters);
#else
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) return interfaces;

    for (auto ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        char ip[INET_ADDRSTRLEN];
        auto sa = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

        if (!isRFC1918(ip)) continue;

        // Get broadcast address for broadcast-capable interfaces
        std::string bcastStr = "255.255.255.255"; // fallback
        if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr != nullptr) {
            char bcast[INET_ADDRSTRLEN];
            auto bsa = (struct sockaddr_in*)ifa->ifa_broadaddr;
            inet_ntop(AF_INET, &bsa->sin_addr, bcast, sizeof(bcast));
            bcastStr = bcast;
        }

        interfaces.push_back({ip, bcastStr});
    }

    freeifaddrs(ifaddr);
#endif

    return interfaces;
}

void NetworkBeacon::updatePayloadString(const std::string& str) {
    // We lock the mutex to ensure the broadcast thread isn't
    // reading the payload while we modify it.
    std::lock_guard<std::mutex> lock(_netMtx);
    _payload = str;
}

std::string NetworkBeacon::buildStandardPayloadPattern(std::string hostname, std::string hostUrl) {
    std::stringstream ss;

    ss << "{";
    ss << "\"service\": \"lemonade\", ";
    ss << "\"hostname\": \"" << hostname << "\", ";
    ss << "\"url\": \"" << hostUrl << "\"";
    ss << "}";

    return ss.str();
}

void NetworkBeacon::startBroadcasting(int beaconPort, int serverPort, uint16_t intervalSeconds) {
    std::lock_guard<std::mutex> lock(_netMtx);

    if (_netThreadRunning) return;

    _port = beaconPort;
    _serverPort = serverPort;
    _broadcastIntervalSeconds = intervalSeconds <= 0 ? 1 : intervalSeconds; //Protect against intervals less than 1
    _netThreadRunning = true;

    _netThread = std::thread(&NetworkBeacon::broadcastThreadLoop, this);
}

void NetworkBeacon::stopBroadcasting() {
    {
        std::lock_guard<std::mutex> lock(_netMtx);
        if (!_netThreadRunning) return;
        _netThreadRunning = false;
    }

    // Join net thread.
    if (_netThread.joinable()) {
        _netThread.join();
    }

    cleanup(); // Close socket after thread is dead
}

void NetworkBeacon::broadcastThreadLoop() {
    int interval;
    int serverPort;
    std::string hostname = getLocalHostname();

    {
        std::lock_guard<std::mutex> lock(_netMtx);
        createSocket();
        int broadcastEnable = 1;
        setsockopt(_socket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));
        interval = _broadcastIntervalSeconds;
        serverPort = _serverPort;
    }

    // Loopback address for same-machine discovery
    sockaddr_in loopbackAddr{};
    loopbackAddr.sin_family = AF_INET;
    loopbackAddr.sin_port = htons(_port);
    loopbackAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(_netMtx);
            if (!_netThreadRunning) break;
        }

        // Enumerate all RFC1918 interfaces and send a beacon on each
        auto interfaces = getLocalRFC1918Interfaces();
        for (const auto& iface : interfaces) {
            // Skip loopback interfaces here, handled separately below
            if (iface.ipAddress.rfind("127.", 0) == 0) continue;

            std::string payload = buildStandardPayloadPattern(
                hostname,
                "http://" + iface.ipAddress + ":" + std::to_string(serverPort) + "/api/v1/"
            );

            sockaddr_in destAddr{};
            destAddr.sin_family = AF_INET;
            destAddr.sin_port = htons(_port);
            inet_pton(AF_INET, iface.broadcastAddress.c_str(), &destAddr.sin_addr);

            auto bytesSent = sendto(_socket, payload.c_str(), (int)payload.size(), 0, (sockaddr*)&destAddr, sizeof(destAddr));
            if (bytesSent < 0) {
                std::cerr << "[Server] [Net Broadcast] sendto failed on " << iface.ipAddress << " (errno: " << errno << ")" << std::endl;
            } else {
                std::cout << "[Server] [Net Broadcast] Beacon sent via " << iface.ipAddress << " -> " << iface.broadcastAddress << ":" << _port << " (" << bytesSent << " bytes)" << std::endl;
            }
        }

        // Always send on loopback for same-machine discovery
        std::string loopbackPayload = buildStandardPayloadPattern(
            hostname,
            "http://127.0.0.1:" + std::to_string(serverPort) + "/api/v1/"
        );
        sendto(_socket, loopbackPayload.c_str(), (int)loopbackPayload.size(), 0, (sockaddr*)&loopbackAddr, sizeof(loopbackAddr));

        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
}
