#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace lemon::backends {
    /**
    * Utility functions for backend management
    */
    class BackendUtils {
    public:
        /**
        * Extract ZIP files (Windows/Linux built-in tools)
        * @param zip_path Path to the ZIP file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_zip(const std::string& zip_path, const std::string& dest_dir, const std::string& backend_name);

        /**
        * Extract tar.gz files (Linux/macOS/Windows)
        * @param tarball_path Path to the tar.gz file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_tarball(const std::string& tarball_path, const std::string& dest_dir, const std::string& backend_name);

        /**
        * Detect if archive is tar or zip
        * @param tarball_path Path to the archive file
        * @param dest_dir Destination directory to extract to
        * @return true if extraction was successful, false otherwise
        */
        static bool extract_archive(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name);
    };
} // namespace lemon::backends
