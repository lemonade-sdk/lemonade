#ifdef __APPLE__

#include "LemonadeServiceManager.h"
#import <Foundation/Foundation.h>
#include <unistd.h> // For usleep
#include "lemon/utils/platform_constants.h"

// Service identifiers
const std::string LemonadeServiceManager::trayServiceID = "com.lemonade.tray";
const std::string LemonadeServiceManager::serverServiceID = "com.lemonade.server";

std::string LemonadeServiceManager::getTargetSpecifier(const std::string& serviceID) {
    if (serviceID == LemonadeServiceManager::trayServiceID) {
        uid_t uid = getuid();
        return "gui/" + std::to_string(uid) + "/" + serviceID;
    } else {
        return "system/" + serviceID;
    }
}

bool LemonadeServiceManager::isTrayActive() {
    // 1. Get the full output of the print command
    std::string output = getLaunchctlOutput("print", getTargetSpecifier(trayServiceID));
    
    // 2. Check if the output actually contains "state = running"
    // "pid = 12345" is also a valid check, but state is more explicit.
    if (output.find("state = running") != std::string::npos) {
        return true;
    }
    return false;
}

bool LemonadeServiceManager::isServerActive() {
    std::string output = getLaunchctlOutput("print", getTargetSpecifier(serverServiceID));
    
    if (output.find("state = running") != std::string::npos) {
        return true;
    }
    return false;
}

bool LemonadeServiceManager::isTrayEnabled() {
    // "Enabled" usually means it's loaded and not disabled.
    // If launchctl print returns ANY output (even if not running), it is Loaded.
    std::string output = getLaunchctlOutput("print", getTargetSpecifier(trayServiceID));
    return !output.empty();
}

bool LemonadeServiceManager::isServerEnabled() {
    std::string output = getLaunchctlOutput("print", getTargetSpecifier(serverServiceID));
    return !output.empty();
}

void LemonadeServiceManager::startServer() {
    std::string target = getTargetSpecifier(serverServiceID);
    std::string plistPath = std::string(PlatformConstants::MACOS_LAUNCH_DAEMON_DIR) 
                        + PlatformConstants::MACOS_BUNDLE_ID 
                        + PlatformConstants::MACOS_PROPERTY_LIST_EXT;
    
    NSLog(@"[Lemonade] Requesting Root privileges to START server...");

    // FIX: Wrap in `sh -c` to isolate exit codes from AppleScript.
    // 1. bootstrap: ">/dev/null 2>&1" silences ALL output/errors. ";" means "proceed even if this fails".
    // 2. enable && kickstart: These run next.
    std::string combinedCmd =
        "sh -c '"
        "/bin/launchctl bootstrap system " + plistPath + " >/dev/null 2>&1; "
        "/bin/launchctl enable " + target + " && "
        "/bin/launchctl kickstart -k " + target + "'";

    if (ExecuteAsRoot(combinedCmd)) {
        NSLog(@"[Lemonade] Server start sequence finished (Admin approved).");
        if (isServerActive()) {
             NSLog(@"[Lemonade] Verification: Server is RUNNING.");
        } else {
             NSLog(@"[Lemonade] Warning: Commands succeeded but server is not yet reporting 'running'.");
        }
    } else {
        NSLog(@"[Lemonade] Failed to start server (Admin denied or Failed).");
    }
}

void LemonadeServiceManager::stopServer() {
    std::string target = getTargetSpecifier(serverServiceID);
    
    if (!isServerActive()) {
        NSLog(@"[Lemonade] Server already inactive. Skipping stop.");
        return;
    }
    
    NSLog(@"[Lemonade] Requesting Root privileges to STOP server...");

    // FIX: Wrap in `sh -c` and use ";" to ensure disable runs even if bootout fails.
    std::string combinedCmd =
        "sh -c '"
        "/bin/launchctl bootout " + target + " >/dev/null 2>&1;'";

    if (ExecuteAsRoot(combinedCmd)) {
        NSLog(@"[Lemonade] Server stop command sent. Waiting for shutdown...");
        
        // Wait loop to verify it actually stops
        int retries = 0;
        while (isServerActive() && retries < PlatformConstants::MACOS_STOP_RETRY_LIMIT) {
            usleep(PlatformConstants::MACOS_STOP_POLL_INTERVAL_USEC);
            retries++;
        }
        if (isServerActive()) {
            NSLog(@"[Lemonade] Server failed to stop within %.1f seconds.", 
                PlatformConstants::MACOS_STOP_TOTAL_TIMEOUT_SEC);
        } else {
            NSLog(@"[Lemonade] Server stopped successfully.");
        }
    } else {
        NSLog(@"[Lemonade] Failed to stop server (Admin denied or Failed).");
    }
}

// MARK: - Private Helper

/**
 Executes a shell command with Root privileges using AppleScript.
 Triggers the macOS Authentication Dialog.
 */
bool LemonadeServiceManager::ExecuteAsRoot(const std::string& command) {
    @autoreleasepool {
        // We must escape quotes in the command string because we are wrapping it in double quotes for AppleScript
        NSString *cmdStr = [NSString stringWithUTF8String:command.c_str()];
        NSString *escapedCmd = [cmdStr stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
        
        NSString *scriptSource = [NSString stringWithFormat:@"do shell script \"%@\" with administrator privileges", escapedCmd];
        
        NSAppleScript *appleScript = [[NSAppleScript alloc] initWithSource:scriptSource];
        NSDictionary *errorInfo = nil;
        
        // Execute (Pauses app until user responds)
        [appleScript executeAndReturnError:&errorInfo];
        
        if (errorInfo) {
            NSNumber *errorNumber = errorInfo[NSAppleScriptErrorNumber];
            NSString *errorMessage = errorInfo[NSAppleScriptErrorMessage];
            
            // Error -128 is "User Cancelled"
            if ([errorNumber intValue] == -128) {
                NSLog(@"[Lemonade] User cancelled password prompt.");
            } else {
                NSLog(@"[Lemonade] Root command failed: %@", errorMessage);
            }
            return false;
        }
        
        return true;
    }
}


void LemonadeServiceManager::enableServer() {
    bool success = runLaunchctlCommand("enable", getTargetSpecifier(serverServiceID));
    if(!success) {
        NSLog(@"[Tray] Server could not be enable successfully, please try again or view the logs and file a issue/report.");
    }
}


void LemonadeServiceManager::disableServer() {
    std::string target = getTargetSpecifier(serverServiceID);
    NSLog(@"[Lemonade] Disabling server service for target: %s", target.c_str());

    bool disableResult = runLaunchctlCommand("disable", target);
    NSLog(@"[Lemonade] disable command result: %s", disableResult ? "SUCCESS" : "FAILED");

    if (disableResult) {
        NSLog(@"[Lemonade] Server service disabled successfully");
    } else {
        NSLog(@"[Lemonade] Failed to disable server service");
    }
}

void LemonadeServiceManager::performFullQuit() {
    NSLog(@"[Lemonade] Performing Full Quit...");
    NSLog(@"[Lemonade] Full Quit Complete.");
}

std::string LemonadeServiceManager::getServerLaunchctlOutput(const std::string& subCmd) {
    return getLaunchctlOutput(subCmd, getTargetSpecifier(serverServiceID));
}


/**
 Runs launchctl and returns TRUE if exit code is 0.
 Used for commands where we don't care about the text output (start, stop, kickstart).
 */
bool LemonadeServiceManager::runLaunchctlCommand(const std::string& subCmd, const std::string& target, const std::string& extraFlag) {
    @autoreleasepool {
        NSTask *task = [[NSTask alloc] init];
        [task setLaunchPath:@"/bin/launchctl"];

        NSMutableArray *args = [NSMutableArray array];
        [args addObject:[NSString stringWithUTF8String:subCmd.c_str()]];
        if (!extraFlag.empty()) {
            [args addObject:[NSString stringWithUTF8String:extraFlag.c_str()]];
        }
        [args addObject:[NSString stringWithUTF8String:target.c_str()]];

        [task setArguments:args];

        NSPipe *errorPipe = [NSPipe pipe];
        [task setStandardOutput:[NSPipe pipe]]; // Silence stdout
        [task setStandardError:errorPipe];

        NSError *error = nil;
        [task launchAndReturnError:&error];
        [task waitUntilExit];

        // Read stderr if command failed
        if ([task terminationStatus] != 0) {
            NSData *errorData = [[errorPipe fileHandleForReading] readDataToEndOfFile];
            NSString *errorString = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
            if (errorString && [errorString length] > 0) {
                NSLog(@"[Lemonade] launchctl error: %s %s %s failed: %@", subCmd.c_str(), extraFlag.c_str(), target.c_str(), errorString);
            }
        }

        return [task terminationStatus] == 0;
    }
}

bool LemonadeServiceManager::runLaunchctlCommand(const std::string& subCmd, const std::string& target) {
    return runLaunchctlCommand(subCmd, target, "");
}

/**
 Runs launchctl and returns the ACTUAL STDOUT string.
 Used for 'print' to check the specific state.
 */
std::string LemonadeServiceManager::getLaunchctlOutput(const std::string& subCmd, const std::string& target) {
    @autoreleasepool {
        NSTask *task = [[NSTask alloc] init];
        [task setLaunchPath:@"/bin/launchctl"];
        
        [task setArguments:@[
            [NSString stringWithUTF8String:subCmd.c_str()],
            [NSString stringWithUTF8String:target.c_str()]
        ]];

        NSPipe *pipe = [NSPipe pipe];
        [task setStandardOutput:pipe];
        [task setStandardError:[NSPipe pipe]]; // Ignore errors

        NSError *error = nil;
        [task launchAndReturnError:&error];
        [task waitUntilExit];
        
        // If command failed (e.g. service not found), return empty string
        if ([task terminationStatus] != 0) {
            return "";
        }

        NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
        NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        
        return [output UTF8String];
    }
}

#endif // __APPLE__
