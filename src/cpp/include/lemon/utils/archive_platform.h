#pragma once

#include <string>
#include <memory>

namespace lemon::utils {

// Abstract interface for platform-specific archive extraction
class ArchivePlatform {
public:
    virtual ~ArchivePlatform() = default;

    // Extract ZIP archive to destination directory
    // Returns true on success, false on failure
    virtual bool extract_zip(const std::string& zip_path,
                            const std::string& dest_dir,
                            const std::string& backend_name) = 0;

    // Extract tarball (.tar.gz, .tar.xz, etc.) to destination directory
    // Returns true on success, false on failure
    virtual bool extract_tarball(const std::string& tarball_path,
                                const std::string& dest_dir,
                                const std::string& backend_name) = 0;

    // Get path to native tar executable (Windows: System32\tar.exe, Unix: tar)
    virtual std::string get_native_tar_path() = 0;

    // Check if native tar is available
    virtual bool is_native_tar_available() = 0;
};

// Factory function to create platform-specific implementation
std::unique_ptr<ArchivePlatform> create_archive_platform();

} // namespace lemon::utils
