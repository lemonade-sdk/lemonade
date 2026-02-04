#pragma once


namespace PlatformConstants {
    // macOS Identity & Paths
    inline constexpr char MACOS_BUNDLE_ID[] = "com.lemonade.server";
    inline constexpr char MACOS_PROPERTY_LIST_EXT[] = ".plist";
    inline constexpr char MACOS_LAUNCH_DAEMON_DIR[] = "/Library/LaunchDaemons/";
    inline constexpr char LAUNCHCTL_PATH[] = "/bin/launchctl";

    // Service Management Timing
    inline constexpr int MACOS_STOP_RETRY_LIMIT = 20;
    inline constexpr int MACOS_STOP_POLL_INTERVAL_USEC = 100000; // 0.1s
    
    // Total timeout for logging purposes (2.0 seconds)
    inline constexpr double MACOS_STOP_TOTAL_TIMEOUT_SEC = (MACOS_STOP_RETRY_LIMIT * MACOS_STOP_POLL_INTERVAL_USEC) / 1000000.0;
}