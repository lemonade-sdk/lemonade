#pragma once

#ifdef __APPLE__

#include <string>

class LemonadeServiceManager {
public:
    // Service status checks
    static bool isTrayActive();
    static bool isServerActive();
    static bool isTrayEnabled();
    static bool isServerEnabled();

    // Service controls
    static void startServer();
    static void stopServer();
    static void enableServer();
    static void disableServer();

    // Combined operations
    static void performFullQuit();

    // Get launchctl output for server service (used by tray app)
    static std::string getServerLaunchctlOutput(const std::string& subCmd);

private:
    static const std::string trayServiceID;
    static const std::string serverServiceID;

    // Helper methods
    static std::string getLaunchctlOutput(const std::string& subCmd, const std::string& target);
    static std::string getTargetSpecifier(const std::string& serviceID);
    static bool ExecuteAsRoot(const std::string& command);
    static bool runLaunchctlCommand(const std::string& subCmd, const std::string& target, const std::string& extraFlag);
    static bool runLaunchctlCommand(const std::string& subCmd, const std::string& target);
};

#endif // __APPLE__
