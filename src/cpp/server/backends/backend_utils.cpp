#include "lemon/backends/backend_utils.h"
#include "lemon/runtime_config.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/whisper_server.h"
#include "lemon/backends/sd_server.h"
#include "lemon/backends/kokoro_server.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/backends/vllm_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/model_manager.h"  // For DownloadProgress, DownloadProgressCallback

#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

using json = nlohmann::json;

namespace lemon::backends {

    const BackendSpec* try_get_spec_for_recipe(const std::string& recipe) {
        if (recipe == "llamacpp") return &LlamaCppServer::SPEC;
        if (recipe == "whispercpp") return &WhisperServer::SPEC;
        if (recipe == "sd-cpp") return &SDServer::SPEC;
        if (recipe == "kokoro") return &KokoroServer::SPEC;
        if (recipe == "ryzenai-llm") return &::lemon::RyzenAIServer::SPEC;
        if (recipe == "vllm") return &VLLMServer::SPEC;
        if (recipe == "flm") return &FastFlowLMServer::SPEC;
        return nullptr;
    }

#ifdef _WIN32
    // Resolve the full path to Windows' built-in bsdtar (System32\tar.exe).
    // This avoids picking up GNU tar from Git, which can't handle zip files
    // and misinterprets drive letter colons as remote host specifiers.
    // Returns "tar" as fallback if SystemRoot isn't set.
    static std::string get_native_tar_path() {
        const char* system_root = std::getenv("SystemRoot");
        if (system_root) {
            return std::string(system_root) + "\\System32\\tar.exe";
        }
        return "tar";
    }

    static bool is_native_tar_available() {
        std::string tar_path = get_native_tar_path();
        std::string command = tar_path + " --version >nul 2>&1";
        std::string unused;
        return lemon::utils::ProcessManager::run_command(command, unused) == 0;
    }
#endif

    bool BackendUtils::extract_zip(const std::string& zip_path, const std::string& dest_dir, const std::string& backend_name) {
        std::string command;
        fs::create_directories(dest_dir);
#ifdef _WIN32
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
#elif defined(__APPLE__) || defined(__linux__)
        LOG(DEBUG, backend_name) << "Extracting zip to " << dest_dir << std::endl;
        command = "unzip -o -q \"" + zip_path + "\" -d \"" + dest_dir + "\"";
#endif
        int result = system(command.c_str());
        if (result != 0) {
            #ifdef _WIN32
                LOG(ERROR, backend_name) << "Extraction failed with code: " << result << std::endl;
            #else
                LOG(ERROR, backend_name) << "Extraction failed. Ensure 'unzip' is installed. Code: " << result << std::endl;
            #endif
            return false;
        }
        return true;
    }

    bool BackendUtils::extract_tarball(const std::string& tarball_path, const std::string& dest_dir, const std::string& backend_name) {
        std::string command;
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting tarball to " << dest_dir << std::endl;
#ifdef _WIN32
        if (!is_native_tar_available()) {
            LOG(ERROR, backend_name) << "Error: 'tar' command not found. Windows 10 (17063+) required." << std::endl;
            return false;
        }
        command = get_native_tar_path() + " -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";
#else
        command = "tar -xzf \"" + tarball_path + "\" -C \"" + dest_dir + "\" --strip-components=1 --no-same-owner";
#endif
        int result = system(command.c_str());
        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result << std::endl;
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
        // Use fs::path throughout to ensure consistent native separators
        fs::path ret = fs::path(utils::get_downloaded_bin_dir()) / dir_name;
        if (!backend.empty()) ret /= backend;
        return ret.make_preferred().string();
    }

    void BackendUtils::build_bin_config_key(const std::string& recipe,
                                              const std::string& backend,
                                              std::string& out_section,
                                              std::string& out_bin_key) {
        std::string config_backend = backend;
        if ((recipe == "llamacpp" || recipe == "sd-cpp") &&
            (backend == "rocm-preview" || backend == "rocm-stable" || backend == "rocm-nightly")) {
            config_backend = "rocm";
        }
        out_section = RuntimeConfig::recipe_to_config_section(recipe);
        out_bin_key = config_backend.empty() ? "server_bin" : (config_backend + "_bin");
    }

    std::string BackendUtils::find_external_backend_binary(const std::string& recipe, const std::string& backend) {
        auto* cfg = lemon::RuntimeConfig::global();
        if (!cfg) return "";

        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);
        std::string bin_value = cfg->backend_string(section, bin_key);

        // Reserved keywords and bare version tags are handled by the install flow.
        if (bin_value.empty() || bin_value == "builtin" || bin_value == "latest") {
            return "";
        }
        if (!utils::looks_like_path(bin_value)) {
            return "";
        }

        RuntimeConfig::validate_bin_path(section, bin_key, bin_value);
        return bin_value;
    }

    std::string BackendUtils::get_bin_config_value(const std::string& recipe, const std::string& backend) {
        auto* cfg = lemon::RuntimeConfig::global();
        if (!cfg) return "";
        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);
        return cfg->backend_string(section, bin_key);
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
        if (backend == "system") {
            // Check if binary exists in PATH
            std::string path = utils::find_executable_in_path(spec.binary);
            if (!path.empty()) {
                return spec.binary;
            }
            throw std::runtime_error(spec.binary + " not found in PATH");
        }

        // Resolve "rocm" to actual channel for backends that support ROCm channels
        std::string resolved_backend = backend;
        if ((spec.recipe == "llamacpp" || spec.recipe == "sd-cpp") && backend == "rocm") {
            std::string channel = "preview";  // default to preview
            if (auto* cfg = RuntimeConfig::global()) {
                channel = cfg->rocm_channel();
            }
            resolved_backend = "rocm-" + channel;
        }

        std::string exe_path = find_external_backend_binary(spec.recipe, resolved_backend);

        if (!exe_path.empty()) {
            return exe_path;
        }

        std::string install_dir = get_install_directory(spec.recipe, resolved_backend);
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
        if (backend == "system") {
            return "";
        }
        std::string install_dir = get_install_directory(spec.recipe, backend);
        return get_version_file(install_dir);
    }

    std::string BackendUtils::get_backend_version(const std::string& recipe, const std::string& backend) {
        std::string resolved_backend = backend;
        if ((recipe == "llamacpp" || recipe == "sd-cpp") && backend == "rocm") {
            // Map "rocm" to the appropriate channel based on config
            std::string channel = "preview";  // default to preview for now
            if (auto* cfg = RuntimeConfig::global()) {
                channel = cfg->rocm_channel();
            }
            resolved_backend = "rocm-" + channel;
        }

        std::string config_path = utils::get_resource_path("resources/backend_versions.json");

        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains(recipe) || !config[recipe].is_object()) {
            throw std::runtime_error("backend_versions.json is missing '" + recipe + "' section");
        }

        const auto& recipe_config = config[recipe];
        const std::string backend_id = recipe + ":" + resolved_backend;

        if (!recipe_config.contains(resolved_backend) || !recipe_config[resolved_backend].is_string()) {
            throw std::runtime_error("backend_versions.json is missing version for backend: " + backend_id);
        }

        std::string version = recipe_config[resolved_backend].get<std::string>();
        return version;
    }

    void BackendUtils::install_from_github(const BackendSpec& spec, const std::string& expected_version, const std::string& repo, const std::string& filename, const std::string& backend, DownloadProgressCallback progress_cb) {
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
                    LOG(INFO, spec.log_name()) << "Upgrading " << spec.binary << " from " << installed_version
                            << " to " << expected_version << std::endl;
                    needs_install = true;
                    fs::remove_all(install_dir);
                }
            }
        }

        if (needs_install) {
            LOG(INFO, spec.log_name()) << "Installing " << spec.binary << " (version: "
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

            LOG(DEBUG, spec.log_name()) << "Downloading from: " << url << std::endl;
            LOG(DEBUG, spec.log_name()) << "Downloading to: " << zip_path << std::endl;

            // Create the appropriate progress callback
            // If an external progress_cb is provided, wrap it as a ProgressCallback for HttpClient
            utils::ProgressCallback http_progress_cb;
            if (progress_cb) {
                http_progress_cb = [&progress_cb, &filename](size_t downloaded, size_t total) -> bool {
                    DownloadProgress p;
                    p.file = filename;
                    p.file_index = 1;
                    p.total_files = 1;
                    p.bytes_downloaded = downloaded;
                    p.bytes_total = total;
                    p.percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
                    p.complete = false;  // Don't signal complete until extraction is done
                    return progress_cb(p);
                };
            } else {
                http_progress_cb = utils::create_throttled_progress_callback();
            }

            // Download the file
            auto download_result = utils::HttpClient::download_file(
                url,
                zip_path,
                http_progress_cb
            );

            if (!download_result.success) {
                // Try split archive download (parts: .part00.tar.gz, .part01.tar.gz, ...)
                // Some backends (e.g. vLLM) exceed GitHub's 2GB release asset limit
                // and are uploaded as split parts.
                bool split_success = false;
                if (is_tarball(filename)) {
                    std::string base = filename.substr(0, filename.size() - 7); // remove .tar.gz
                    std::string base_url = "https://github.com/" + repo + "/releases/download/" +
                                          expected_version + "/";
                    LOG(DEBUG, spec.log_name()) << "Single file download failed, trying split parts..." << std::endl;

                    // Open combined output file
                    std::ofstream combined(zip_path, std::ios::binary);
                    int part_num = 0;
                    while (true) {
                        char part_suffix[16];
                        snprintf(part_suffix, sizeof(part_suffix), ".part%02d.tar.gz", part_num);
                        std::string part_filename = base + part_suffix;
                        std::string part_url = base_url + part_filename;
                        std::string part_path = zip_path + ".part" + std::to_string(part_num);

                        LOG(DEBUG, spec.log_name()) << "Trying part: " << part_filename << std::endl;

                        auto part_result = utils::HttpClient::download_file(
                            part_url, part_path,
                            utils::create_throttled_progress_callback()
                        );

                        if (!part_result.success) {
                            fs::remove(part_path);
                            break; // No more parts
                        }

                        // Append part to combined file
                        std::ifstream part_in(part_path, std::ios::binary);
                        combined << part_in.rdbuf();
                        part_in.close();
                        fs::remove(part_path);
                        part_num++;
                    }
                    combined.close();

                    if (part_num > 0) {
                        LOG(INFO, spec.log_name()) << "Downloaded " << part_num << " split parts" << std::endl;
                        split_success = true;
                    } else {
                        fs::remove(zip_path);
                    }
                }

                if (!split_success) {
                    throw std::runtime_error("Failed to download " + spec.binary + " from: " + url +
                                            " - " + download_result.error_message);
                }
            }

            LOG(DEBUG, spec.log_name()) << "Download complete!" << std::endl;

            // Verify the downloaded file
            if (!fs::exists(zip_path)) {
                throw std::runtime_error("Downloaded archive does not exist: " + zip_path);
            }

            std::uintmax_t file_size = fs::file_size(zip_path);
            LOG(DEBUG, spec.log_name()) << "Downloaded archive file size: "
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
                LOG(ERROR, spec.log_name()) << "Extraction completed but executable not found" << std::endl;
                fs::remove(zip_path);
                fs::remove_all(install_dir);
                throw std::runtime_error("Extraction failed: executable not found");
            }

            LOG(DEBUG, spec.log_name()) << "Executable verified at: " << exe_path << std::endl;

            // Save version info
            std::ofstream vf(version_file);
            vf << expected_version;
            vf.close();

    #ifndef _WIN32
            // Make all binaries in bin/ executable (tar may lose permissions)
            {
                auto bin_dir = fs::path(install_dir) / "bin";
                if (fs::exists(bin_dir)) {
                    for (auto& entry : fs::directory_iterator(bin_dir)) {
                        if (entry.is_regular_file()) {
                            chmod(entry.path().c_str(), 0755);
                        }
                    }
                }
            }
            // Also make the found executable itself executable
            chmod(exe_path.c_str(), 0755);
    #endif

            // Delete ZIP file
            fs::remove(zip_path);

            // Send completion event now that installation is fully done
            if (progress_cb) {
                DownloadProgress p;
                p.file = filename;
                p.file_index = 1;
                p.total_files = 1;
                p.bytes_downloaded = download_result.bytes_downloaded;
                p.bytes_total = download_result.total_bytes;
                p.percent = 100;
                p.complete = true;
                progress_cb(p);
            }

            LOG(DEBUG, spec.log_name()) << "Installation complete!" << std::endl;
        } else {
            LOG(DEBUG, spec.log_name()) << "Found executable at: " << exe_path << std::endl;

            // Even if already installed, send a completion event so callers know it's done
            if (progress_cb) {
                DownloadProgress p;
                p.file = filename;
                p.file_index = 1;
                p.total_files = 1;
                p.bytes_downloaded = 0;
                p.bytes_total = 0;
                p.percent = 100;
                p.complete = true;
                progress_cb(p);
            }
        }
    }
    bool BackendUtils::is_rocm_installed_system_wide() {
#ifndef __linux__
        return false;
#else
        // Only check /opt/rocm for system-wide installation
        // (/usr is handled by the system backend separately)
        fs::path rocm_root("/opt/rocm");

        // Check for libamdhip64.so in lib directories
        std::vector<std::string> lib_subdirs = {"lib", "lib64"};
        bool found_lib = false;

        for (const auto& lib_subdir : lib_subdirs) {
            fs::path lib_path = rocm_root / lib_subdir / "libamdhip64.so";
            if (fs::exists(lib_path)) {
                found_lib = true;
                break;
            }
        }

        if (!found_lib) {
            LOG(DEBUG, "BackendUtils") << "No system-wide ROCm installation detected at /opt/rocm" << std::endl;
            return false;
        }

        // Verify with version file
        std::vector<std::string> version_paths = {
            (rocm_root / ".info" / "version").string(),
            (rocm_root / "share" / "rocm" / "version").string(),
            (rocm_root / "version").string()
        };

        for (const auto& version_path : version_paths) {
            if (fs::exists(version_path)) {
                LOG(DEBUG, "BackendUtils") << "Found system ROCm at /opt/rocm with version file: "
                          << version_path << std::endl;
                return true;
            }
        }

        // If we found the lib but no version file, log a warning but still accept it
        LOG(DEBUG, "BackendUtils") << "Found ROCm libraries at /opt/rocm (no version file found)" << std::endl;
        return true;
#endif
    }

    std::string BackendUtils::get_therock_install_dir(const std::string& arch, const std::string& version) {
        fs::path therock_base = fs::path(utils::get_downloaded_bin_dir()) / "therock";
        return (therock_base / (arch + "-" + version)).string();
    }

    void BackendUtils::cleanup_old_therock_versions(const std::string& current_version) {
#ifdef __linux__
        fs::path therock_base = fs::path(utils::get_downloaded_bin_dir()) / "therock";

        if (!fs::exists(therock_base)) {
            return;
        }

        try {
            for (const auto& entry : fs::directory_iterator(therock_base)) {
                if (entry.is_directory()) {
                    std::string dir_name = entry.path().filename().string();
                    size_t dash_pos = dir_name.rfind('-');
                    if (dash_pos != std::string::npos) {
                        std::string version = dir_name.substr(dash_pos + 1);
                        if (version != current_version) {
                            LOG(DEBUG, "BackendUtils") << "Cleaning up old TheRock version: " << dir_name << std::endl;
                            fs::remove_all(entry.path());
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "BackendUtils") << "Failed to cleanup old TheRock versions: " << e.what() << std::endl;
        }
#endif
    }

    void BackendUtils::install_therock(const std::string& arch, const std::string& version,
                                       DownloadProgressCallback progress_cb) {
#if !defined(__linux__) && !defined(_WIN32)
        throw std::runtime_error("TheRock is only supported on Linux and Windows");
#else
        std::string install_dir = get_therock_install_dir(arch, version);
        std::string version_file = (fs::path(install_dir) / "version.txt").string();

        if (fs::exists(install_dir) && fs::exists(version_file)) {
            std::string installed_version;
            std::ifstream vf(version_file);
            std::getline(vf, installed_version);
            vf.close();

            if (installed_version == version) {
                LOG(DEBUG, "BackendUtils") << "TheRock " << arch << "-" << version << " already installed" << std::endl;
                return;
            }
        }

        LOG(INFO, "BackendUtils") << "Installing TheRock ROCm " << version << " for " << arch << std::endl;

        fs::create_directories(install_dir);

        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        json config = utils::JsonUtils::load_from_file(config_path);

        std::string url_variant = arch;
        if (config.contains("therock") && config["therock"].contains("url_mapping") &&
            config["therock"]["url_mapping"].contains(arch)) {
            url_variant = config["therock"]["url_mapping"][arch].get<std::string>();
        }

#ifdef _WIN32
        std::string platform = "windows";
#else
        std::string platform = "linux";
#endif
        std::string filename = "therock-dist-" + platform + "-" + url_variant + "-" + version + ".tar.gz";
        std::string url = "https://repo.amd.com/rocm/tarball/" + filename;

        fs::path cache_dir = fs::temp_directory_path();
        std::string tarball_path = (cache_dir / filename).string();

        LOG(DEBUG, "BackendUtils") << "Downloading TheRock from: " << url << std::endl;
        LOG(DEBUG, "BackendUtils") << "Downloading to: " << tarball_path << std::endl;

        // Create progress callback for download
        utils::ProgressCallback http_progress_cb;
        if (progress_cb) {
            http_progress_cb = [&progress_cb, &filename](size_t downloaded, size_t total) -> bool {
                DownloadProgress p;
                p.file = filename;
                p.file_index = 2;  // TheRock is the second file (after llama.cpp binary)
                p.total_files = 2;
                p.bytes_downloaded = downloaded;
                p.bytes_total = total;
                p.percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
                p.complete = false;
                return progress_cb(p);
            };
        } else {
            http_progress_cb = utils::create_throttled_progress_callback();
        }

        auto download_result = utils::HttpClient::download_file(
            url,
            tarball_path,
            http_progress_cb
        );

        if (!download_result.success) {
            throw std::runtime_error("Failed to download TheRock from: " + url +
                                    " - " + download_result.error_message);
        }

        LOG(DEBUG, "BackendUtils") << "TheRock download complete" << std::endl;

        if (!fs::exists(tarball_path)) {
            throw std::runtime_error("Downloaded TheRock tarball does not exist: " + tarball_path);
        }

        std::uintmax_t file_size = fs::file_size(tarball_path);
        LOG(DEBUG, "BackendUtils") << "Downloaded tarball size: "
                                    << (file_size / 1024 / 1024) << " MB" << std::endl;

        if (!extract_tarball(tarball_path, install_dir, "TheRock")) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract TheRock tarball: " + tarball_path);
        }

#ifdef _WIN32
        // On Windows, DLLs are in bin/ (lib/ contains only import .lib files)
        fs::path runtime_dir = fs::path(install_dir) / "bin";
        if (!fs::exists(runtime_dir)) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("TheRock extraction failed: bin directory not found");
        }
        LOG(DEBUG, "BackendUtils") << "TheRock bin directory verified at: " << runtime_dir << std::endl;
#else
        // On Linux, shared libraries are in lib/
        fs::path runtime_dir = fs::path(install_dir) / "lib";
        if (!fs::exists(runtime_dir)) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("TheRock extraction failed: lib directory not found");
        }
        LOG(DEBUG, "BackendUtils") << "TheRock lib directory verified at: " << runtime_dir << std::endl;
#endif

        std::ofstream vf(version_file);
        vf << version;
        vf.close();

        fs::remove(tarball_path);
        cleanup_old_therock_versions(version);

        // Send completion notification
        if (progress_cb) {
            DownloadProgress p;
            p.file = filename;
            p.file_index = 2;  // TheRock is the second file
            p.total_files = 2;
            p.bytes_downloaded = download_result.bytes_downloaded;
            p.bytes_total = download_result.total_bytes;
            p.percent = 100;
            p.complete = true;
            progress_cb(p);
        }

        LOG(INFO, "BackendUtils") << "TheRock installation complete" << std::endl;
#endif
    }

    std::string BackendUtils::get_therock_lib_path(const std::string& rocm_arch) {
#if !defined(__linux__) && !defined(_WIN32)
        return "";
#else
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains("therock") || !config["therock"].contains("version")) {
            throw std::runtime_error("backend_versions.json is missing 'therock.version'");
        }

        std::string version = config["therock"]["version"].get<std::string>();

        // Only return the path if TheRock is already installed
        std::string install_dir = get_therock_install_dir(rocm_arch, version);
        if (fs::exists(install_dir)) {
#ifdef _WIN32
            // On Windows, DLLs are in bin/ (lib/ contains only import .lib files)
            std::string lib_path = (fs::path(install_dir) / "bin").string();
#else
            // On Linux, shared libraries are in lib/
            std::string lib_path = (fs::path(install_dir) / "lib").string();
#endif
            LOG(DEBUG, "BackendUtils") << "Returning TheRock runtime path: " << lib_path << std::endl;
            return lib_path;
        }

        return "";
#endif
    }
} // namespace lemon::backends
