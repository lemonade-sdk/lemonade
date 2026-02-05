#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace lemon::backends {
    struct BackendSpec {
        const std::string log_name;
        const std::string recipe;
        const std::string binary;
        const std::string dir_name;
        BackendSpec(const std::string l,
            const std::string r,
            const std::string b,
            const std::string d): log_name(l), recipe(r), binary(b), dir_name(d) {}
    };

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

        // Excluding from lemonade-server to avoid having to compile in additional transitive dependencies
    #ifndef LEMONADE_TRAY
        static void install_from_github(const BackendSpec& spec, const std::string& expected_version, const std::string& repo, const std::string& filename, const std::string& variant);
        static std::string get_backend_version(const std::string& recipe, const std::string& variant);
    #endif

        static std::string get_backend_binary_path(const BackendSpec& spec, const std::string& variant);

        static std::string get_install_directory(const std::string& dir_name, const std::string& variant);
        static std::string find_executable_in_install_dir(const std::string& install_dir, const std::string& binary_name);
        static std::string find_external_backend_binary(const std::string& recipe, const std::string& variant);
    };
} // namespace lemon::backends
