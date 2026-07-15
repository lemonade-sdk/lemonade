#include <lemon/utils/archive_platform.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/aixlog.hpp>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <process.h>

namespace fs = std::filesystem;

namespace lemon::utils {

namespace {
std::string escape_powershell_literal(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\'') {
            escaped += "''";
        } else {
            escaped += c;
        }
    }
    return escaped;
}
} // namespace

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
        try {
            return ProcessManager::run_process_with_output(
                tar_path,
                {"--version"},
                [](const std::string&) { return true; },
                "",
                5
            ) == 0;
        } catch (const std::exception&) {
            return false;
        }
    }
    bool extract_zip(const std::string& zip_path,
                    const std::string& dest_dir,
                    const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        std::string output;
        int result;

        if (is_native_tar_available()) {
            LOG(DEBUG, backend_name) << "Extracting ZIP with native tar to " << dest_dir << std::endl;
            result = ProcessManager::run_process_with_output(
                get_native_tar_path(),
                {"-xf", zip_path, "-C", dest_dir},
                [&output](const std::string& line) {
                    output += line + "\n";
                    return true;
                },
                "",
                300
            );
        } else {
            LOG(DEBUG, backend_name) << "Extracting ZIP via PowerShell to " << dest_dir << std::endl;
            std::string powershell_path = "powershell";
            const char* system_root = std::getenv("SystemRoot");
            if (system_root) {
                powershell_path = std::string(system_root) + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
            }
            result = ProcessManager::run_process_with_output(
                powershell_path,
                {"-Command", "Expand-Archive -LiteralPath '" + escape_powershell_literal(zip_path) +
                 "' -DestinationPath '" + escape_powershell_literal(dest_dir) + "' -Force"},
                [&output](const std::string& line) {
                    output += line + "\n";
                    return true;
                },
                "",
                300
            );
        }

        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result
                                    << (output.empty() ? "" : " - " + output) << std::endl;
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

        std::string entries;
        int list_result = ProcessManager::run_process_with_output(
            get_native_tar_path(),
            {"-tf", tarball_path},
            [&entries](const std::string& line) {
                entries += line + "\n";
                return true;
            },
            "",
            30,
            false
        );

        int strip = 0;
        if (list_result == 0) {
            strip = compute_tarball_strip_components(entries);
            LOG(DEBUG, backend_name) << "Tarball strip-components: " << strip << std::endl;
        } else {
            LOG(DEBUG, backend_name) << "Could not list tarball contents, using strip=0" << std::endl;
        }

        std::string output;
        int result = ProcessManager::run_process_with_output(
            get_native_tar_path(),
            {"-xf", tarball_path, "-C", dest_dir,
             "--strip-components=" + std::to_string(strip), "--no-same-owner"},
            [&output](const std::string& line) {
                output += line + "\n";
                return true;
            },
            "",
            300
        );

        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result
                                    << (output.empty() ? "" : " - " + output) << std::endl;
            return false;
        }
        return true;
    }
};

std::unique_ptr<ArchivePlatform> create_archive_platform() {
    return std::make_unique<WindowsArchivePlatform>();
}

} // namespace lemon::utils
