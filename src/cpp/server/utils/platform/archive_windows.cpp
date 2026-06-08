#include <lemon/utils/archive_platform.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace lemon::utils {

class WindowsArchivePlatform : public ArchivePlatform {
public:
    std::string get_native_tar_path() override {
        const char* system_root = std::getenv("SystemRoot");
        if (system_root) {
            return std::string(system_root) + "\\System32\\tar.exe";
        }
        return "tar";
    }

    bool is_native_tar_available() override {
        std::string tar_path = get_native_tar_path();
        std::string command = tar_path + " --version >nul 2>&1";
        std::string unused;
        return ProcessManager::run_command(command, unused) == 0;
    }
    bool extract_zip(const std::string& zip_path,
                    const std::string& dest_dir,
                    const std::string& backend_name) override {
        std::string command;
        fs::create_directories(dest_dir);

        if (is_native_tar_available()) {
            LOG(DEBUG, backend_name) << "Extracting ZIP with native tar to " << dest_dir << std::endl;
            command = get_native_tar_path() + " -xf \"" + zip_path + "\" -C \"" + dest_dir + "\"";
        } else {
            LOG(DEBUG, backend_name) << "Extracting ZIP via PowerShell to " << dest_dir << std::endl;
            std::string powershell_path = "powershell";
            const char* system_root = std::getenv("SystemRoot");
            if (system_root) {
                powershell_path = std::string(system_root) + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
            }
            command = powershell_path + " -Command \"Expand-Archive -Path '" + zip_path +
                     "' -DestinationPath '" + dest_dir + "' -Force\"";
        }

        int result = system(command.c_str());
        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result << std::endl;
            return false;
        }
        return true;
    }

    bool extract_tarball(const std::string& tarball_path,
                        const std::string& dest_dir,
                        const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting tarball to " << dest_dir << std::endl;

        if (!is_native_tar_available()) {
            LOG(ERROR, backend_name) << "Error: 'tar' command not found. Windows 10 (17063+) required." << std::endl;
            return false;
        }

        std::string command = get_native_tar_path() + " -xf \"" + tarball_path +
                            "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";

        int result = system(command.c_str());
        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result << std::endl;
            return false;
        }
        return true;
    }
};

std::unique_ptr<ArchivePlatform> create_archive_platform() {
    return std::make_unique<WindowsArchivePlatform>();
}

} // namespace lemon::utils
