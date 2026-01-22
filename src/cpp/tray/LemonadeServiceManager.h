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

private:
    static const std::string trayServiceID;
    static const std::string serverServiceID;

    // Helper methods
    static std::string getLaunchctlOutput(const std::string& subCmd, const std::string& target);
    static std::string getTargetSpecifier(const std::string& serviceID);
    static bool ExecuteAsRoot(const std::string& command);
    static bool checkServiceStatus(const std::string& serviceID);
    static bool checkServiceEnabled(const std::string& serviceID);
    static void enableService(const std::string& serviceID);
    static void disableService(const std::string& serviceID);
    static void kickstartService(const std::string& serviceID);
    static void bootoutService(const std::string& serviceID);
    static bool runLaunchctlCommand(const std::string& subCmd, const std::string& target, const std::string& extraFlag);
    static bool runLaunchctlCommand(const std::string& subCmd, const std::string& target);
};

#endif // __APPLE__
