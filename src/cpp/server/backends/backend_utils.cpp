#include "lemon/backends/backend_utils.h"

#include "lemon/utils/path_utils.h"
#include <filesystem>
#include <iostream>
#include <cstdlib>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

namespace lemon::backends {
    bool BackendUtils::extract_zip(const std::string& zip_path, const std::string& dest_dir, const std::string& backend_name) {
        std::string command;
#ifdef _WIN32
        std::string mkdir_cmd = "if not exist \"" + dest_dir + "\" mkdir \"" + dest_dir + "\" >nul 2>&1";
        system(mkdir_cmd.c_str());
#else
        std::string mkdir_cmd = "mkdir -p \"" + dest_dir + "\"";
        system(mkdir_cmd.c_str());
#endif
#ifdef _WIN32
        // Check if 'tar' is available (Windows 10 build 17063+ ships with bsdtar)
        int tar_check = system("tar --version >nul 2>&1");
        if (tar_check == 0) {
            std::cout << "[" << backend_name << "] Extracting ZIP with native tar to " << dest_dir << std::endl;
            // -x: extract, -f: file, -C: change dir
            command = "tar -xf \"" + zip_path + "\" -C \"" + dest_dir + "\"";
        } else {
            std::cout << "[" << backend_name << "] Extracting ZIP via PowerShell to " << dest_dir << std::endl;
            // PowerShell fallback - use full path to avoid PATH issues
            std::string powershell_path = "powershell";
            const char* system_root = std::getenv("SystemRoot");
            if (system_root) {
                powershell_path = std::string(system_root) + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
            }
            command = powershell_path + " -Command \"Expand-Archive -Path '" + zip_path + 
                    "' -DestinationPath '" + dest_dir + "' -Force\"";
        }
#elif defined(__APPLE__) || defined(__linux__)
        // macOS & Linux Logic
        std::cout << "[" << backend_name << "] Extracting zip to " << dest_dir << std::endl;
        command = "unzip -o -q \"" + zip_path + "\" -d \"" + dest_dir + "\"";
#endif
        int result = system(command.c_str());
        if (result != 0) {
            // Adjust error message based on platform context
            #ifdef _WIN32
                std::cerr << "[" << backend_name << "] Extraction failed with code: " << result << std::endl;
            #else
                std::cerr << "[" << backend_name << "] Extraction failed. Ensure 'unzip' is installed. Code: " << result << std::endl;
            #endif
            return false;
        }
        return true;
    }

    bool BackendUtils::extract_tarball(const std::string& tarball_path, const std::string& dest_dir, const std::string& backend_name) {
        std::string command;
        int result;
#ifdef _WIN32
        // Windows: Use 'if not exist' to avoid errors if it already exists
        std::string mkdir_cmd = "if not exist \"" + dest_dir + "\" mkdir \"" + dest_dir + "\" >nul 2>&1";
        if (system(mkdir_cmd.c_str()) != 0) {
            std::cerr << "[" << backend_name << "] Failed to create directory: " << dest_dir << std::endl;
            return false;
        }
#else
        // Linux/macOS: 'mkdir -p' creates parents and is silent if exists
        std::string mkdir_cmd = "mkdir -p \"" + dest_dir + "\"";
        if (system(mkdir_cmd.c_str()) != 0) {
            std::cerr << "[" << backend_name << "] Failed to create directory: " << dest_dir << std::endl;
            return false;
        }
#endif
        std::cout << "[" << backend_name << "] Extracting tarball to " << dest_dir << std::endl;
#ifdef _WIN32
        // Windows 10/11 ships with 'bsdtar' as 'tar.exe'.
        // It natively supports gzip (-z) and --strip-components.
        
        // Check if tar exists first
        int tar_check = system("tar --version >nul 2>&1");
        if (tar_check != 0) {
            std::cerr << "[" << backend_name << "] Error: 'tar' command not found. Windows 10 (17063+) required." << std::endl;
            return false;
        }
        // Command structure is identical to Linux for modern Windows tar
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1";
#elif defined(__APPLE__)
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1";

#else
        // Linux (uses GNU tar by default)
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1";
#endif
        result = system(command.c_str());
        if (result != 0) {
            std::cerr << "[" << backend_name << "] Extraction failed with code: " << result << std::endl;
            return false;
        }
        return true;
    }

    // Helper to extract archive files based on extension
    bool BackendUtils::extract_archive(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name) {
        // Check if it's a tar.gz file
        if (archive_path.size() > 7 && 
            archive_path.substr(archive_path.size() - 7) == ".tar.gz") {
            return extract_tarball(archive_path, dest_dir, backend_name);
        }
        // Default to ZIP extraction
        return extract_zip(archive_path, dest_dir, backend_name);
    }

} // namespace lemon::backends