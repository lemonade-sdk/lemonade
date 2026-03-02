#ifndef NETWORK_BEACON_H
#define NETWORK_BEACON_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>

    typedef int socklen_t;
#else
    typedef int SOCKET;
#endif

struct NetworkInterfaceInfo {
    std::string ipAddress;
    std::string broadcastAddress;
};

class NetworkBeacon {
public:
    NetworkBeacon();
    ~NetworkBeacon();

    // Server: Starts a blocking loop to shout presence
    std::string getLocalHostname();
    std::string buildStandardPayloadPattern(std::string hostname, std::string hostUrl);
    bool isRFC1918(const std::string& ipAddress);
    std::vector<NetworkInterfaceInfo> getLocalRFC1918Interfaces();
    void startBroadcasting(int beaconPort, int serverPort, uint16_t intervalSeconds);
    void stopBroadcasting();

private:
    std::mutex _netMtx;
    std::thread _netThread;
    std::atomic<bool> _netThreadRunning = false;

    uint16_t _port;
    uint16_t _serverPort;
    SOCKET _socket;
    bool _isInitialized;
    uint16_t _broadcastIntervalSeconds;
    void cleanup();
    void createSocket();
    void broadcastThreadLoop();
};

#endif
