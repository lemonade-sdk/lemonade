#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace lemon::backends {
    struct BackendSpec {
        const std::string recipe;
        const std::string binary;
        BackendSpec(const std::string r, const std::string b): recipe(r), binary(b) {}

        std::string log_name() const { return recipe + " Server"; };
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
        /** Download and install the specified version of the backend from github */
        static void install_from_github(const BackendSpec& spec, const std::string& expected_version, const std::string& repo, const std::string& filename, const std::string& backend);

        /** Get the latest version number for the given recipe/backend */
        static std::string get_backend_version(const std::string& recipe, const std::string& backend);
    #endif

        /** Get the path to the backend's binary. Gives precedence to the path set through environment variables, if set. Throws if not found. */
        static std::string get_backend_binary_path(const BackendSpec& spec, const std::string& backend);

        /** Get the path where the version indicator is installed. Does not check existence. */
        static std::string get_installed_version_file(const BackendSpec& spec, const std::string& backend);

        /** Get the install directory for the backend. Generally only used internally by BackendUtils */
        static std::string get_install_directory(const std::string& dir_name, const std::string& backend);

        /** Find the executable in the installation directory. Generally only used internally by BackendUtils */
        static std::string find_executable_in_install_dir(const std::string& install_dir, const std::string& binary_name);

        /** Checks the environment for a variable following the scheme LEMONADE_BACKEND_VARIANT_BIN and return its value, if available. Generally only used internally by BackendUtils */
        static std::string find_external_backend_binary(const std::string& recipe, const std::string& backend);
    };
} // namespace lemon::backends
