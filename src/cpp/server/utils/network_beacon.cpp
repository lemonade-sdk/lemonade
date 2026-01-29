#include "lemon/utils/network_beacon.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <stdexcept>

#ifdef _WIN32
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define closesocket close
    #define SOCKET_ERROR -1
#endif

NetworkBeacon::NetworkBeacon() : _socket(INVALID_SOCKET), _isInitialized(false), _netThreadRunning(false) {
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
    if (_socket != INVALID_SOCKET) {
        closesocket(_socket);
        _socket = INVALID_SOCKET;
    }
#ifdef _WIN32
    if (_isInitialized) {
        WSACleanup();
        _isInitialized = false;
    }
#endif
}

void NetworkBeacon::createSocket() {
    if (_socket != INVALID_SOCKET) {
        closesocket(_socket);
    }
    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_socket == INVALID_SOCKET) {
        throw std::runtime_error("Could not create socket");
    }
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