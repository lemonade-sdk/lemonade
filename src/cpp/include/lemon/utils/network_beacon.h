#ifndef NETWORK_BEACON_H
#define NETWORK_BEACON_H

#include <cstdint>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    typedef int socklen_t;
#else
    typedef int SOCKET;
    #define INVALID_SOCKET -1
#endif

class NetworkBeacon {
public:
    NetworkBeacon();
    ~NetworkBeacon();

    // Server: Starts a blocking loop to shout presence
    void startBroadcasting(int port, const std::string& payload, uint16_t intervalSeconds);
    void stopBroadcasting();

private:
    std::mutex _netMtx;
    std::thread _netThread;
    std::atomic<bool> _netThreadRunning = false;

    uint16_t _port;
    SOCKET _socket;
    bool _isInitialized;
    uint16_t _broadcastIntervalSeconds;
    std::string _payload;

    void cleanup();
    void createSocket();
    void broadcastThreadLoop();
};

#endif