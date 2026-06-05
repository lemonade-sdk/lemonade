/// \file main.cpp
/// \brief Main entry point for the FLM application
/// \author FastFlowLM Team
/// \date 2025-08-05
/// \version 0.9.24
/// \note This is a source file for the main entry point
#pragma once
#include "runner.hpp"
#include "server.hpp"
#include "model_list.hpp"
#include "model_downloader.hpp"
#include "update.hpp"
#include "utils/utils.hpp"
#include "program_args.hpp"
#include "minja/chat-template.hpp"
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <io.h>
#include "utils/wmi_helper.hpp"
#endif
#include "utils/vm_args.hpp"
#include <boost/program_options.hpp>
#include "benchmarking.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <linux/types.h>
#include <libdrm/drm.h>
#include <sys/utsname.h>
#include <cerrno>
#include "npu_utils/amdxdna_accel.h"
#endif

#include "AutoModel/automodel.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#endif

// Global variables
///@brief should_exit is used to control the server thread
std::atomic<bool> should_exit(false);
std::mutex exit_mutex;
std::condition_variable exit_cv;

#ifndef _WIN32
///@brief Preload critical XRT libraries from the executable directory
///@details This ensures that dlopen() calls within libraries find the bundled versions
///@note Only on Linux/Unix; Windows handles DLL loading differently
void preload_bundled_libraries() {
    std::string exe_dir = utils::get_executable_directory();

    // When running inside a snap, LD_LIBRARY_PATH already points at the
    // bundled XRT libs (e.g. $SNAP/usr/lib/x86_64-linux-gnu).  Passing a
    // bare library name lets the dynamic linker resolve it via
    // LD_LIBRARY_PATH, which avoids hard-coding the arch triplet.
    // Outside of a snap the libs are expected to live next to the binary.
    const char* snap_env = std::getenv("SNAP");
    std::string lib_prefix = snap_env && *snap_env ? "" : exe_dir + "/";

    const std::vector<std::string> libraries = {
        "libxrt_core.so.2",           // Core - no dependencies
        "libxrt_coreutil.so.2",       // Depends on core
        "libxrt_driver_xdna.so.2",    // Driver
    };

    // Try to load the library with RTLD_GLOBAL so symbols are available to dependent libraries
    // Don't care about failures
    for (const auto& lib : libraries) {
        std::string lib_path = lib_prefix + lib;
        void* handle = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    }
}
#endif



#ifdef _WIN32
///@brief get_unicode_command_line_args gets Unicode command line arguments
///@param argc_out reference to store argument count
///@return vector of UTF-8 encoded argument strings
std::vector<std::string> get_unicode_command_line_args(int& argc_out) {
    std::vector<std::string> args;

    // Get the Unicode command line
    LPWSTR* szArglist;
    int nArgs;

    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (szArglist == nullptr) {
        argc_out = 0;
        return args;
    }

    argc_out = nArgs;

    // Convert each argument from wide string to UTF-8
    for (int i = 0; i < nArgs; i++) {
        std::wstring warg(szArglist[i]);

        // Convert to UTF-8
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, warg.c_str(), (int)warg.size(), nullptr, 0, nullptr, nullptr);
        if (size_needed > 0) {
            std::string utf8_arg(size_needed, 0);
            WideCharToMultiByte(CP_UTF8, 0, warg.c_str(), (int)warg.size(), &utf8_arg[0], size_needed, nullptr, nullptr);
            args.push_back(utf8_arg);
        } else {
            args.push_back(""); // fallback for conversion error
        }
    }

    // Free memory allocated by CommandLineToArgvW
    LocalFree(szArglist);

    return args;
}
#endif

///@brief get_user_documents_directory gets the user's Documents directory
///@return the user's Documents directory path


///@brief ensure_models_directory creates the models directory if it doesn't exist
///@param exe_dir the executable directory
void ensure_models_directory(const std::string& exe_dir) {
    // Use Documents/flm/models directory on Windows or ~/.config/flm on Linux for models instead of executable directory
    std::string models_dir = utils::get_models_directory();
    if (!std::filesystem::exists(models_dir)) {
        std::filesystem::create_directories(models_dir);
    }
}

std::atomic<bool> running(true);
std::condition_variable cv;
std::mutex mtx;
void signal_handler(int signal) {
    if (signal == SIGINT) {
        running = false;
        cv.notify_one();
    }
}

///@brief create_lm_server is used to create the ollama server
///@param models the model list
///@param default_tag the default tag
///@param port the port to listen on, default is 52625, same with the ollama server
///@return the server
std::unique_ptr<WebServer> create_lm_server(model_list& models, ModelDownloader& downloader, program_args_t& args);

#ifdef _WIN32
std::string get_driver_version(const std::string& device_name) {
    std::string driver_version;
    wmi::WMIConnection wmi;
    if (!wmi.is_valid()) {
        return "";
    }

    std::wstring query = L"SELECT * FROM Win32_PnPSignedDriver WHERE DeviceName LIKE '%" +
        wmi::string_to_wstring(device_name) + L"%'";

    wmi.query(query, [&driver_version](IWbemClassObject* pObj) {
        if (driver_version.empty()) {  // Only get first match
            driver_version = wmi::get_property_string(pObj, L"DriverVersion");
        }
        });

    return driver_version;
}

std::string identify_npu_arch() {
    wmi::WMIConnection wmi_conn;
    if (!wmi_conn.is_valid()) {
        return "";
    }

    // XDNA2 NPU: AMD vendor 1022, device 17F0
    bool found_xdna2 = false;
    wmi_conn.query(
        L"SELECT PNPDeviceID FROM Win32_PnPEntity WHERE PNPDeviceID LIKE '%VEN_1022&DEV_17F0%'",
        [&found_xdna2](IWbemClassObject* pObj) {
            found_xdna2 = true;
        });

    if (found_xdna2) {
        return "XDNA2";
    }
    return "";
}

#endif

static bool sanity_check_npu_stack(bool quiet, bool json_output = false) {
    bool print_human = !quiet && !json_output;
#ifndef _WIN32
    nlohmann::json validation_json = {
        {"object", "npu_stack_validation"},
        {"platform", "windows"},
        {"kernel_ok", true},
        {"amd_device_found", true},
        {"all_fw_ok", true},
        {"enough_cols", true},
        {"memlock_ok", true},
        {"devices", nlohmann::json::array()},
        {"ready", true}
    };
    validation_json["platform"] = "linux";
    // Check kernel version
    struct utsname u_name;
    if (uname(&u_name) != 0) {
        if (print_human)
            perror("Failed to get kernel version");
        validation_json["kernel_ok"] = false;
        validation_json["ready"] = false;
        if (json_output) {
            std::cout << validation_json.dump(4) << std::endl;
        }
        return false;
    }
    int major, minor;
    sscanf(u_name.release, "%d.%d", &major, &minor);
    bool kernel_ok = (major > 6) || (major == 6 && minor >= 17);
    validation_json["kernel"] = u_name.release;
    validation_json["kernel_ok"] = kernel_ok;
    if (!kernel_ok) {
        if (print_human) {
            header_print_r("ERROR", "Kernel version incompatible with this version of FLM. Please update your kernel!");
        }
        validation_json["ready"] = false;
        if (json_output) {
            std::cout << validation_json.dump(4) << std::endl;
        }
        return false;
    }
    if (print_human) {
        header_print("Linux", "Kernel: " << u_name.release);
    }

    // Check firmware version of all AMD devices
    bool all_fw_ok = true;
    bool enough_cols = true;
    bool amd_device_found = false;
    bool drm_version_ok = true;

    for (int i = 0; i < 16; ++i) {
        std::string dev_name = "/dev/accel/accel" + std::to_string(i);
        int fd = open(dev_name.c_str(), O_RDWR);

        if (fd < 0) {
            if (errno == ENOENT)
                break;
            continue;
        }

        struct drm_version drm_v;
        memset(&drm_v, 0, sizeof(drm_v));
        std::string drm_version_str = "unknown";
        if (ioctl(fd, DRM_IOCTL_VERSION, &drm_v) == 0) {
            drm_version_str = std::to_string(drm_v.version_major) + "." + std::to_string(drm_v.version_minor);
            if (!(drm_v.version_major > 0 || (drm_v.version_major == 0 && drm_v.version_minor >= 6))) {
                drm_version_ok = false;
            }
        } else {
            drm_version_ok = false;
        }
        validation_json["drm_version"] = drm_version_str;

        amdxdna_drm_query_firmware_version query_fw_version;
        amdxdna_drm_get_info get_info = {
            .param = DRM_AMDXDNA_QUERY_FIRMWARE_VERSION,
            .buffer_size = sizeof(amdxdna_drm_query_firmware_version),
            .buffer = (unsigned long)&query_fw_version,
        };

        int ret = ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info);
        if (ret < 0) {
            close(fd);
            // ENOTTY means it's not an AMD device, just ignore it.
            if (errno != ENOTTY) {
                 if (!quiet) {
                    header_print_r("ERROR", "Error code: " << ret << " on " << dev_name);
                    perror("Failed to get firmware version");
                 }
            }
            continue;
        }

        amd_device_found = true;

        nlohmann::json device_entry = {
            {"device", dev_name},
            {"fw_major", query_fw_version.major},
            {"fw_minor", query_fw_version.minor},
            {"fw_patch", query_fw_version.patch},
            {"fw_build", query_fw_version.build}
        };

        bool fw_ok = (query_fw_version.major > 1 || (query_fw_version.major == 1 && query_fw_version.minor >= 1));
        device_entry["fw_ok"] = fw_ok;
        if (!fw_ok) {
            all_fw_ok = false;
            if (print_human) {
                header_print_r("ERROR", "NPU firmware version on " + dev_name + " is incompatible. Please update NPU firmware!");
            }
        }

        amdxdna_drm_query_aie_metadata query_aie_metadata;
        get_info.param = DRM_AMDXDNA_QUERY_AIE_METADATA;
        get_info.buffer_size = sizeof(amdxdna_drm_query_aie_metadata);
        get_info.buffer = (unsigned long)&query_aie_metadata;
        if (ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &get_info) == 0) {
            device_entry["cols"] = query_aie_metadata.cols;
            if (query_aie_metadata.cols < 8) {
                enough_cols = false;
                if (print_human) {
                    header_print_r("ERROR", "NPU " + dev_name + " does not have enough columns (" << query_aie_metadata.cols << " < 8)");
                }
            }
        }
        close(fd);

        if (print_human) {
            header_print_g("Linux", "NPU: " + dev_name + " with " + std::to_string(query_aie_metadata.cols) + " columns");
            header_print_g("Linux", "NPU FW Version: " << query_fw_version.major << "." << query_fw_version.minor << "." << query_fw_version.patch << "." << query_fw_version.build);
        }

        validation_json["devices"].push_back(device_entry);
    }
    validation_json["amd_device_found"] = amd_device_found;
    validation_json["all_fw_ok"] = all_fw_ok;
    validation_json["enough_cols"] = enough_cols;

    if (amd_device_found) {
        if (!drm_version_ok)
            kernel_ok = false;
        if (print_human) {
            if (drm_version_ok)
                header_print_g("Linux", "amdxdna version: " << validation_json["drm_version"].get<std::string>());
            else
                header_print_r("ERROR", "amdxdna version " << validation_json["drm_version"].get<std::string>() << " is incompatible");
        }
    }
    validation_json["kernel_ok"] = kernel_ok;

    if (!amd_device_found && print_human) {
        header_print_r("ERROR", "No NPU device found.");
    }

    // Check memlock limit
    bool memlock_ok = true;
    struct rlimit rl;
    if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
        validation_json["memlock_limit"] = (rl.rlim_cur == RLIM_INFINITY)
            ? "infinity"
            : std::to_string(static_cast<unsigned long long>(rl.rlim_cur));
        if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 100 * 1024 * 1024) {
            struct rlimit rl_new = { RLIM_INFINITY, RLIM_INFINITY };
            if (setrlimit(RLIMIT_MEMLOCK, &rl_new) == 0) {
                rl.rlim_cur = RLIM_INFINITY;
                rl.rlim_max = RLIM_INFINITY;
                validation_json["memlock_limit"] = "infinity";
                if (print_human)
                    header_print_g("Linux", "Memlock Limit: set to infinity");
            } else {
                if (print_human) {
                    header_print_r("ERROR", "Memlock limit is too low (" << (rl.rlim_cur / 1024 / 1024) << "MB). Please raise the limit or set to infinity.");
                }
                memlock_ok = false;
            }
        } else if (print_human) {
            if (rl.rlim_cur == RLIM_INFINITY) {
                header_print_g("Linux", "Memlock Limit: infinity");
            } else {
                header_print("Linux", "Memlock Limit: " << (rl.rlim_cur / 1024 / 1024) << " MB");
            }
        }
    } else {
        if (print_human)
            perror("Failed to get memlock limit");
    }
    validation_json["memlock_ok"] = memlock_ok;

    bool overall_ok = amd_device_found && kernel_ok && all_fw_ok && enough_cols && memlock_ok;
    validation_json["ready"] = overall_ok;
    if (json_output) {
        std::cout << validation_json.dump(4) << std::endl;
    }

    return overall_ok;
#else
    nlohmann::json validation_json = {
        {"object", "npu_stack_validation"},
        {"platform", "windows"},
        {"amd_device_found", true},
        {"npu_driver_ok", true},
        {"ready", true}
    };
    std::string npu_arch = identify_npu_arch();
    if (npu_arch.empty()) {
        if (print_human)
            header_print("Error", "No XDNA2 NPU hardware detected");
        validation_json["ready"] = false;
        validation_json["amd_device_found"] = false;
        if (json_output) {
            std::cout << validation_json.dump(4) << std::endl;
        }
        return false;
    }

    std::string min_drv = __NPU_VERSION__;
    std::string drv = get_driver_version("NPU Compute Accelerator Device");
    int mver_0, mver_1, mver_2, mver_3;
    int ver_0, ver_1, ver_2, ver_3;
    sscanf(min_drv.c_str(), "%d.%d.%d.%d", &mver_0, &mver_1, &mver_2, &mver_3);
    sscanf(drv.c_str(), "%d.%d.%d.%d", &ver_0, &ver_1, &ver_2, &ver_3);

    if (ver_3 < mver_3) {
        if (print_human) {
            header_print("Error", "NPU driver version doesn't meet the minimum!");
        }
        validation_json["ready"] = false;
        validation_json["npu_driver_ok"] = false;
        if (json_output) {
            std::cout << validation_json.dump(4) << std::endl;
        }
        return false;
    }

    if (print_human) {
        header_print_g("Windows", "NPU: " << npu_arch);
        header_print_g("Windows", "NPU dirver version: " << drv);
    }

    if (json_output) {
        std::cout << validation_json.dump(4) << std::endl;
    }
    return true;
#endif
}


///@brief main function
///@param argc the number of arguments
///@param argv the arguments
///@return the exit code
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    // Preload bundled libraries from executable directory
    preload_bundled_libraries();
#endif

    // Parse command line arguments using Boost Program Options
    program_args_t parsed_args;
    if (!arg_utils::parse_options(argc, argv, parsed_args)) {
        return 1; // Help was already printed by Boost Program Options
    }


    // Get the command, model tag, and force flag
    std::string exe_dir = utils::get_executable_directory();
    std::string config_path = utils::find_model_list();
    // Get the models directory from environment variable or default
    std::string models_dir = utils::get_models_directory();


    model_list availble_models(config_path, models_dir);

    // Extract parsed values
    bool got_power_mode = (parsed_args.power_mode != "performance"); // Check if user explicitly set power mode
    bool stable_stack = false;

    // Set process priority to high for better performance
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    // Check if the commands and args valid
    if (parsed_args.command == "validate") {
        stable_stack = sanity_check_npu_stack(parsed_args.command != "validate", parsed_args.command == "validate" && parsed_args.json_output);
        return stable_stack ? 0 : 1;
    }

    if (parsed_args.command == "run" || parsed_args.command == "serve" || parsed_args.command == "pull" || parsed_args.command == "remove" || parsed_args.command == "check" || parsed_args.command == "bench") {
      if (parsed_args.model_tag != "model-faker" && (!availble_models.is_model_supported(parsed_args.model_tag))) {
            header_print("ERROR", "Model not found: " << parsed_args.model_tag << "; Please check with `flm list` and try again.");
            return 1;
        }
    }

    if (parsed_args.command == "serve" || parsed_args.command == "run" || parsed_args.command == "bench"){
        // Configure AMD XRT for the specified power mode
        if (parsed_args.power_mode == "default" || parsed_args.power_mode == "powersaver" || parsed_args.power_mode == "balanced" ||
            parsed_args.power_mode == "performance" || parsed_args.power_mode == "turbo") {
#ifdef _WIN32
            std::string xrt_cmd = "cd \"C:\\Windows\\System32\\AMD\" && .\\xrt-smi.exe configure --pmode " + parsed_args.power_mode + " > NUL 2>&1";
            header_print("FLM", "Configuring NPU Power Mode to " + parsed_args.power_mode + (got_power_mode ? "" : " (flm default)"));
            (void)system(xrt_cmd.c_str());
#endif
        }
        else{
            std::cout << "Invalid power mode: " << parsed_args.power_mode << std::endl;
            std::cout << "Valid power modes: default, powersaver, balanced, performance, turbo" << std::endl;
            return 1;
        }
    }

    // Handle special case for serve command - use default tag if none provided
    if (parsed_args.command == "serve" && parsed_args.model_tag.empty()) {
        parsed_args.model_tag = "model-faker"; // Use default tag
    }

#ifndef _WIN32
    // Raise memlock limit to accommodate the model being loaded
    if ((parsed_args.command == "run" || parsed_args.command == "serve" || parsed_args.command == "bench")) {
        rlim_t model_size = 0;
        rlim_t asr_size = 0;
        rlim_t embedding_size = 0;

        if (parsed_args.model_tag != "model-faker" && !parsed_args.model_tag.empty()) {
            auto [resolved_tag, model_info] = availble_models.get_model_info(parsed_args.model_tag);
            if (model_info.contains("size")) {
                model_size = model_info["size"].get<uint64_t>();
            }
        }
        if (parsed_args.asr) {
            asr_size = 1000000000;
            header_print_g("FLM", "ASR mode enabled: reserving additional 1GB of memory");
        }
        if (parsed_args.embed) {
            embedding_size = 300000000;
            header_print_g("FLM", "Embedding mode enabled: reserving additional 300MB of memory");
        }

        // Add 512MB overhead for working memory beyond the model itself
        rlim_t required = model_size + asr_size + embedding_size + (512ULL * 1024 * 1024);
        struct rlimit rl;
        if (getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
            if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < required) {
                struct rlimit rl_new = { required, required };
                if (setrlimit(RLIMIT_MEMLOCK, &rl_new) == 0) {
                    header_print_g("Linux", "Memlock Limit: raised to " << (required / 1024 / 1024) << " MB");
                } else {
                    header_print("Linux", "Warning: could not raise memlock limit to " << (required / 1024 / 1024) << " MB");
                }
            }
        }
    }
#endif

    // code for all commands:

    if (parsed_args.command == "version") {
        if (parsed_args.json_output) {
            std::cout << "{ \"version\": \"" << __FLM_VERSION__ << "\" }" << std::endl;
        } else {
            std::cout << "FLM v" << __FLM_VERSION__ << std::endl;
        }
        return 0;
    }

    if (parsed_args.command == "port"){
        if (parsed_args.json_output) {
            std::cout << "{ \"port\": " << utils::get_server_port(parsed_args.port) << " }" << std::endl;
        } else {
            std::cout << "Server Port: " << utils::get_server_port(parsed_args.port) << std::endl;
        }
        return 0;
    }

    if (parsed_args.preemption){
        header_print("FLM", "Allowing high priority tasks to preempt FLM!");
    }


    try {

        // Load the model list with the models directory as the base
        ModelDownloader downloader(availble_models);

        // Ensure models directory exists
        if (!std::filesystem::exists(models_dir)) {
            std::filesystem::create_directories(models_dir);
        }

        if (parsed_args.command == "bench") {
            benchmarking::BenchmarkResults_t results = benchmarking::run_benchmarks(parsed_args.model_tag, parsed_args.input_file_name, availble_models);
        }
        else if (parsed_args.command == "run") {
            check_and_notify_new_version();
            Runner runner(availble_models, downloader, parsed_args);
            runner.run();

        } else if (parsed_args.command == "serve") {
            check_and_notify_new_version();
            // Create the server
            int port = utils::get_server_port(parsed_args.port);
            if (parsed_args.port == -1) { // User did not specify port
                header_print("FLM", "Using environment-specified port: " << port);
                parsed_args.port = port; // overwrite to ensure server uses correct port
            } else {
                header_print("FLM", "Using user-specified port: " << port);
            }
            auto server = create_lm_server(availble_models, downloader, parsed_args);
            server->set_max_connections(parsed_args.max_socket_connections);           // Allow up to 10 concurrent connections
            server->set_io_threads(10);          // Allow up to 5 io threads
            server->set_npu_queue_length(parsed_args.max_npu_queue);           // Allow up to 10 concurrent queue
            server->set_request_timeout(std::chrono::seconds(600)); // 10 minute timeout for long requests
            // Start the server
            header_print("FLM", "Starting server on port " << port << "...");
            server->start();
            header_print("FLM", "Press Ctrl+C to stop.");
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [] { return !running.load(); });
            }
            // header_print("FLM", "Stopping server...");
            // server->stop();
        }
        else if (parsed_args.command == "pull") {
            // Check if the model is already downloaded, if true, the model will not be downloaded
            // Check if model is already downloaded
            if (!parsed_args.force_redownload && downloader.is_model_downloaded(parsed_args.model_tag)) {
                header_print("FLM", "Model is already downloaded.");
                // Show missing files if any, this will be used to show the missing files
                auto missing_files = downloader.get_missing_files(parsed_args.model_tag);
                if (!missing_files.empty()) {
                    header_print("FLM", "Missing files:");
                    for (const auto& file : missing_files) {
                        std::cout << "  - " << file << std::endl;
                    }
                } else {
                    header_print("FLM", "All required files are present.");
                }
            } else {
                // Download the model, this will be used to download the model
                bool success = downloader.pull_model(parsed_args.model_tag, parsed_args.force_redownload);
                if (!success) {
                    header_print("ERROR", "Failed to pull model: " + parsed_args.model_tag);
                    return 1;
                }
            }
        }
        else if (parsed_args.command == "remove") {
            // Remove the model, this will be used to remove the model
            downloader.remove_model(parsed_args.model_tag);
        }
        else if (parsed_args.command == "check") {
            downloader.check_model(parsed_args.model_tag);
        }
        else if (parsed_args.command == "list") {
            // List the models, this will be used to list the models
            if (!(parsed_args.list_filter == "installed" || parsed_args.list_filter == "not-installed" || parsed_args.list_filter == "all")) {
                header_print("Error", "Invalid filter: please use 'all', 'installed', or 'not-installed'");
                return 1;
            }
            if (parsed_args.json_output) {
                nlohmann::json output_json;
                output_json["models"] = nlohmann::json::array();
                nlohmann::json models = availble_models.get_all_models();
                for (const auto& model : models["models"]) {
                    // Fast path: `list` only needs presence + compatibility,
                    // so skip HF metadata fetch and per-file hash verification.
                    bool is_present = downloader.is_model_downloaded(model["name"].get<std::string>(), /*parsed_args.sub_process_mode*/true, /*fast_check=*/true);
                    if ((parsed_args.list_filter == "installed") == is_present || parsed_args.list_filter == "all") {
                        nlohmann::json model_entry = model;
                        model_entry["installed"] = is_present ? true : false;
                        output_json["models"].push_back(model_entry);
                    }
                }
                std::cout << output_json.dump(4) << std::endl;
            }
            else {
                std::cout << "Models:" << std::endl;
                nlohmann::json models = availble_models.get_all_models();
                bool any_model_listed = false;
                for (const auto& model : models["models"]) {
                    // Fast path: skip HF metadata fetch and per-file hash verification.
                    bool is_present = downloader.is_model_downloaded(model["name"].get<std::string>(), parsed_args.sub_process_mode, /*fast_check=*/true);
                    if ((parsed_args.list_filter == "installed") == is_present || parsed_args.list_filter == "all") {
                        std::cout << "  - " << model["name"].get<std::string>();
                        if (!parsed_args.sub_process_mode) {
                            std::cout << (is_present ? " ✅" : " ⏬");
                        }
                        std::cout << std::endl;
                        any_model_listed = true;
                    }
                }
                if (!any_model_listed) {
                    std::cout << "  No models found for the specified filter." << std::endl;
                }
            }
        }
        else {
            // Invalid command, this will be used to show the invalid command
            std::cerr << "Invalid command: " << parsed_args.command << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            return 1;
        }
        // Return 0 if the command is valid
        return 0;
    } catch (const std::exception& e) {
        // If an error occurs, this will be used to show the error
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
