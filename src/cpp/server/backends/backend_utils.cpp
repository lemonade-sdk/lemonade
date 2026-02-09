#include "lemon/backends/backend_utils.h"

#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

using json = nlohmann::json;

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
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";
#elif defined(__APPLE__)
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";

#else
        // Linux (uses GNU tar by default)
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";
#endif
        result = system(command.c_str());
        if (result != 0) {
            std::cerr << "[" << backend_name << "] Extraction failed with code: " << result << std::endl;
            return false;
        }
        return true;
    }

    static bool is_tarball(const std::string& filename) {
        return (filename.size() > 7) && (filename.substr(filename.size() - 7) == ".tar.gz");
    }

    // Helper to extract archive files based on extension
    bool BackendUtils::extract_archive(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name) {
        // Check if it's a tar.gz file
        if (is_tarball(archive_path)) {
            return extract_tarball(archive_path, dest_dir, backend_name);
        }
        // Default to ZIP extraction
        return extract_zip(archive_path, dest_dir, backend_name);
    }

    std::string BackendUtils::get_install_directory(const std::string& dir_name, const std::string& backend) {
        fs::path ret = (fs::path(utils::get_downloaded_bin_dir()) / dir_name);
        return ((backend != "") ? (ret / backend) : ret).string();
    }

    std::string BackendUtils::find_external_backend_binary(const std::string& recipe, const std::string& backend) {
        std::string upper = backend == "" ? recipe : (recipe + "_" + backend);
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        std::string env = "LEMONADE_" + upper + "_BIN";
        const char* backend_bin_env = std::getenv(env.c_str());
        if (!backend_bin_env) {
            return "";
        }

        std::string backend_bin = std::string(backend_bin_env);
        return fs::exists(backend_bin) ? backend_bin : "";
    }

    std::string BackendUtils::find_executable_in_install_dir(const std::string& install_dir, const std::string& binary_name) {
        if (fs::exists(install_dir)) {
            // This could be optimized with a cache but saving a few milliseconds every few minutes/hours is not going to do much
            for (const fs::directory_entry& dir_entry : fs::recursive_directory_iterator(install_dir)) {
                if (dir_entry.is_regular_file() && dir_entry.path().filename() == binary_name) {
                    return dir_entry.path().string();
                }
            }
        }

        return "";
    }

    std::string BackendUtils::get_backend_binary_path(const BackendSpec& spec, const std::string& backend) {
        std::string exe_path = find_external_backend_binary(spec.recipe, backend);

        if (!exe_path.empty()) {
            return exe_path;
        }

        std::string install_dir = get_install_directory(spec.recipe, backend);
        exe_path = find_executable_in_install_dir(install_dir, spec.binary);

        if (!exe_path.empty()) {
            return exe_path;
        }

        // If not found, throw error with helpful message
        throw std::runtime_error(spec.binary + " not found in install directory: " + install_dir);
    }

    static std::string get_version_file(std::string& install_dir) {
        return (fs::path(install_dir) / "version.txt").string();
    }

    std::string BackendUtils::get_installed_version_file(const BackendSpec& spec, const std::string& backend) {
        std::string install_dir = get_install_directory(spec.recipe, backend);
        return get_version_file(install_dir);
    }

#ifndef LEMONADE_TRAY
    std::string BackendUtils::get_backend_version(const std::string& recipe, const std::string& backend) {
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");

        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains(recipe) || !config[recipe].is_object()) {
            throw std::runtime_error("backend_versions.json is missing '" + recipe + "' section");
        }

        const auto& recipe_config = config[recipe];
        const std::string backend_id = recipe + ":" + backend;

        if (!recipe_config.contains(backend) || !recipe_config[backend].is_string()) {
            throw std::runtime_error("backend_versions.json is missing version for backend: " + backend_id);
        }

        std::string version = recipe_config[backend].get<std::string>();
        std::cout << "[BackendUtils] Using " << backend_id << " version from config: " << version << std::endl;
        return version;
    }

    void BackendUtils::install_from_github(const BackendSpec& spec, const std::string& expected_version, const std::string& repo, const std::string& filename, const std::string& backend) {
        std::string install_dir;
        std::string version_file;
        std::string exe_path = find_external_backend_binary(spec.recipe, backend);
        bool needs_install = exe_path.empty();

        if (needs_install) {
            install_dir = get_install_directory(spec.recipe, backend);
            version_file = get_version_file(install_dir);

            // Check if already installed with correct version
            exe_path = find_executable_in_install_dir(install_dir, spec.binary);
            needs_install = exe_path.empty();

            if (!needs_install && fs::exists(version_file)) {
                std::string installed_version;

                std::ifstream vf(version_file);
                std::getline(vf, installed_version);
                vf.close();

                if (installed_version != expected_version) {
                    std::cout << "[" << spec.log_name() << "] Upgrading " << spec.binary << " from " << installed_version
                            << " to " << expected_version << std::endl;
                    needs_install = true;
                    fs::remove_all(install_dir);
                }
            }
        }

        if (needs_install) {
            std::cout << "[" << spec.log_name() << "] Installing " << spec.binary << " (version: "
                    << expected_version << ")" << std::endl;

            // Create install directory
            fs::create_directories(install_dir);

            std::string url = "https://github.com/" + repo + "/releases/download/" +
                            expected_version + "/" + filename;

            // Download ZIP to cache directory
            fs::path cache_dir = fs::temp_directory_path();
            fs::create_directories(cache_dir);
            std::string zip_name = backend == "" ? spec.recipe : spec.recipe + "_" + backend;
            std::string zip_ext = is_tarball(filename) ? ".tar.gz" : ".zip";
            std::string zip_path = (cache_dir / (zip_name + "_" + expected_version + zip_ext)).string();

            std::cout << "[" << spec.log_name() << "] Downloading from: " << url << std::endl;
            std::cout << "[" << spec.log_name() << "] Downloading to: " << zip_path << std::endl;

            // Download the file
            auto download_result = utils::HttpClient::download_file(
                url,
                zip_path,
                utils::create_throttled_progress_callback()
            );

            if (!download_result.success) {
                throw std::runtime_error("Failed to download " + spec.binary + " from: " + url +
                                        " - " + download_result.error_message);
            }

            std::cout << std::endl << "[" << spec.log_name() << "] Download complete!" << std::endl;

            // Verify the downloaded file
            if (!fs::exists(zip_path)) {
                throw std::runtime_error("Downloaded archive does not exist: " + zip_path);
            }

            std::uintmax_t file_size = fs::file_size(zip_path);
            std::cout << "[" << spec.log_name() << "] Downloaded archive file size: "
                    << (file_size / 1024 / 1024) << " MB" << std::endl;

            // Extract
            if (!extract_archive(zip_path, install_dir, spec.log_name())) {
                fs::remove(zip_path);
                fs::remove_all(install_dir);
                throw std::runtime_error("Failed to extract archive: " + zip_path);
            }

            // Verify extraction
            exe_path = find_executable_in_install_dir(install_dir, spec.binary);
            if (exe_path.empty()) {
                std::cerr << "[" << spec.log_name() << "] ERROR: Extraction completed but executable not found" << std::endl;
                fs::remove(zip_path);
                fs::remove_all(install_dir);
                throw std::runtime_error("Extraction failed: executable not found");
            }

            std::cout << "[" << spec.log_name() << "] Executable verified at: " << exe_path << std::endl;

            // Save version info
            std::ofstream vf(version_file);
            vf << expected_version;
            vf.close();

    #ifndef _WIN32
            // Make executable on Linux/macOS
            chmod(exe_path.c_str(), 0755);
    #endif

            // Delete ZIP file
            fs::remove(zip_path);

            std::cout << "[" << spec.log_name() << "] Installation complete!" << std::endl;
        } else {
            std::cout << "[" << spec.log_name() << "] Found executable at: " << exe_path << std::endl;
        }
    }
#endif
} // namespace lemon::backends
