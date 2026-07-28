#include <lemon/utils/archive_platform.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/aixlog.hpp>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace lemon::utils {

class UnixArchivePlatform : public ArchivePlatform {
public:
    bool extract_zip(const std::string& zip_path,
                    const std::string& dest_dir,
                    const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting zip to " << dest_dir << std::endl;

        std::string output;
        int result = ProcessManager::run_process_with_output(
            "unzip",
            {"-o", "-q", zip_path, "-d", dest_dir},
            [&output](const std::string& line) {
                output += line + "\n";
                return true;
            },
            "",
            30
        );

        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed. Ensure 'unzip' is installed. Code: "
                                    << result << (output.empty() ? "" : " - " + output) << std::endl;
            return false;
        }
        return true;
    }

    bool extract_tarball(const std::string& tarball_path,
                         const std::string& dest_dir,
                         const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting tarball to " << dest_dir << std::endl;

        std::string entries;
        int list_result = ProcessManager::run_process_with_output(
            "tar",
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
            "tar",
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

    std::string get_native_tar_path() override {
        return "tar";
    }

    bool is_native_tar_available() override {
        try {
            return ProcessManager::run_process_with_output(
                "tar",
                {"--version"},
                [](const std::string&) { return true; },
                "",
                5
            ) == 0;
        } catch (const std::exception&) {
            return false;
        }
    }
};

std::unique_ptr<ArchivePlatform> create_archive_platform() {
    return std::make_unique<UnixArchivePlatform>();
}

} // namespace lemon::utils
