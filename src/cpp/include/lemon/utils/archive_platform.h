#pragma once

#include <sstream>
#include <string>
#include <memory>

namespace lemon::utils {

// Decide the tar --strip-components value from a newline-separated `tar -tf`
// listing. Strips one level only when every entry lives under a single shared
// top-level directory that also contains nested entries; otherwise keeps
// root-level entries intact (strip 0). A bare top-level directory entry (e.g.
// "pkg/") is treated as that shared directory, not as a root-level file, so
// listings that include an explicit directory entry are handled the same as
// those that omit it.
inline int compute_tarball_strip_components(const std::string& entries) {
    std::string first_dir;
    bool all_same_dir = true;
    bool has_nested = false;

    std::istringstream iss(entries);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        const bool is_dir_entry = line.back() == '/';
        std::string entry = line;
        if (is_dir_entry) {
            entry.pop_back();
        }
        auto pos = entry.find('/');
        if (pos == std::string::npos) {
            // A root-level file blocks stripping; a bare top-level directory
            // entry ("pkg/") just names the shared directory.
            if (!is_dir_entry) {
                all_same_dir = false;
            } else if (first_dir.empty()) {
                first_dir = entry;
            } else if (entry != first_dir) {
                all_same_dir = false;
            }
        } else {
            std::string dir = entry.substr(0, pos);
            if (first_dir.empty()) {
                first_dir = dir;
            } else if (dir != first_dir) {
                all_same_dir = false;
            }
            has_nested = true;
        }
    }

    return (all_same_dir && !first_dir.empty() && has_nested) ? 1 : 0;
}

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
