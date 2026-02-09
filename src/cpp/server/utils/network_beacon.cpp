#include "lemon/utils/network_beacon.h"

#include <chrono>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define INVALID_SOCKET_NB INVALID_SOCKET
#else
    #include <arpa/inet.h>
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

    return false;
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

void NetworkBeacon::startBroadcasting(int port, const std::string& payload, uint16_t intervalSeconds) {
    std::lock_guard<std::mutex> lock(_netMtx);
    
    if (_netThreadRunning) return; 

    _port = port;
    _payload = payload;
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
    // Setup - Localize data to minimize lock time
    sockaddr_in addr{};
    std::string currentPayload;
    int interval;

    {
        std::lock_guard<std::mutex> lock(_netMtx);
        createSocket();
        int broadcastEnable = 1;
        setsockopt(_socket, SOL_SOCKET, SO_BROADCAST, (char*)&broadcastEnable, sizeof(broadcastEnable));
        
        addr.sin_family = AF_INET;
        addr.sin_port = htons(_port);
        addr.sin_addr.s_addr = INADDR_BROADCAST;
        
        currentPayload = _payload;
        interval = _broadcastIntervalSeconds;
    }
    
    while (true) 
    {
        {
            std::lock_guard<std::mutex> lock(_netMtx);
            if (!_netThreadRunning) break;
            currentPayload = _payload; // Allow payload updates on the fly
        }

        sendto(_socket, currentPayload.c_str(), (int)currentPayload.size(), 0, (sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
}
