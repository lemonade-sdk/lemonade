#include <lemon/utils/archive_platform.h>
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace lemon::utils {

class UnixArchivePlatform : public ArchivePlatform {
public:
    bool extract_zip(const std::string& zip_path,
                    const std::string& dest_dir,
                    const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting zip to " << dest_dir << std::endl;

        std::string command = "unzip -o -q \"" + zip_path + "\" -d \"" + dest_dir + "\"";
        int result = system(command.c_str());

        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed. Ensure 'unzip' is installed. Code: "
                                    << result << std::endl;
            return false;
        }
        return true;
    }

    bool extract_tarball(const std::string& tarball_path,
                        const std::string& dest_dir,
                        const std::string& backend_name) override {
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting tarball to " << dest_dir << std::endl;

        // Use auto-detect form `-xf` for .tar.gz, .tar.xz, .tar.bz2, etc.
        std::string command = "tar -xf \"" + tarball_path + "\" -C \"" + dest_dir +
                            "\" --strip-components=1 --no-same-owner";

        int result = system(command.c_str());
        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result << std::endl;
            return false;
        }
        return true;
    }

    std::string get_native_tar_path() override {
        return "tar";
    }

    bool is_native_tar_available() override {
        return system("tar --version >/dev/null 2>&1") == 0;
    }
};

std::unique_ptr<ArchivePlatform> create_archive_platform() {
    return std::make_unique<UnixArchivePlatform>();
}

} // namespace lemon::utils
