#include "lemon/backends/fastflowlm/fastflowlm_models.h"

#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>
#include "lemon/model_manager.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <sstream>
#include <thread>
#include <chrono>
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/system_info.h"
#include "lemon/utils/process_manager.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {
namespace backends {
namespace fastflowlm {
namespace {

using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

bool safe_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Candidate roots that FLM may use to store models. FLM resolves its model
// directory from the FLM_MODEL_PATH env var (set by the installer) and falls
// back to platform-default locations.
std::vector<fs::path> get_flm_models_dir_candidates() {
    std::vector<fs::path> roots;

    const char* flm_model_path = std::getenv("FLM_MODEL_PATH");
    if (flm_model_path && *flm_model_path) {
        roots.push_back(path_from_utf8(flm_model_path) / "models");
    }

#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && *userprofile) {
        fs::path home = path_from_utf8(userprofile);
        roots.push_back(home / ".flm" / "models");              // current installer default
        roots.push_back(home / "Documents" / "flm" / "models"); // legacy installer default
        roots.push_back(home / "flm" / "models");
    }
#else
    const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config_home && *xdg_config_home) {
        roots.push_back(path_from_utf8(xdg_config_home) / "flm" / "models");
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        fs::path home_path = path_from_utf8(home);
        roots.push_back(home_path / ".flm" / "models");
        roots.push_back(home_path / ".config" / "flm" / "models");
    }
#endif

    return roots;
}

} // namespace

fs::path find_flm_config_path_from_repo_dir(const std::string& repo_dir) {
    if (repo_dir.empty()) return fs::path();

    for (const auto& root : get_flm_models_dir_candidates()) {
        fs::path candidate = root / repo_dir / "config.json";
        if (safe_exists(candidate)) return candidate;
    }
    return fs::path();
}

std::string repo_dir_from_url(const std::string& url) {
    std::string clean = url;
    while (!clean.empty() && clean.back() == '/') clean.pop_back();
    size_t query_pos = clean.find_first_of("?#");
    if (query_pos != std::string::npos) clean = clean.substr(0, query_pos);

    for (const std::string marker : {"/tree/", "/resolve/"}) {
        size_t marker_pos = clean.find(marker);
        if (marker_pos != std::string::npos) {
            clean = clean.substr(0, marker_pos);
            break;
        }
    }

    size_t slash = clean.find_last_of('/');
    return slash == std::string::npos ? clean : clean.substr(slash + 1);
}

int64_t read_flm_max_context_window(const ModelInfo& info) {
    if (info.type != ModelType::LLM) return 0;

    std::string config_path = info.resolved_path("config");
    if (config_path.empty()) return 0;

    try {
        json config = lemon::utils::JsonUtils::load_from_file(config_path);
        if (config.contains("max_position_embeddings") && config["max_position_embeddings"].is_number_integer()) {
            int64_t value = config["max_position_embeddings"].get<int64_t>();
            return value > 0 ? value : 0;
        }
        if (config.contains("text_config") && config["text_config"].is_object()) {
            const auto& text_config = config["text_config"];
            if (text_config.contains("max_position_embeddings") && text_config["max_position_embeddings"].is_number_integer()) {
                int64_t value = text_config["max_position_embeddings"].get<int64_t>();
                return value > 0 ? value : 0;
            }
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "FastFlowLM") << "Could not read FLM config metadata for "
                                 << info.model_name << ": " << e.what() << std::endl;
    }
    return 0;
}

std::string find_flm_binary() {
    try {
        const backends::BackendSpec* spec = try_get_spec_for_recipe("flm");
        if (!spec) {
            return "";
        }
        return BackendUtils::get_backend_binary_path(*spec, "npu");
    } catch (...) {
#ifndef _WIN32
        return find_flm_executable();
#else
        return "";
#endif
    }
}

std::vector<std::string> flm_installed_checkpoints() {
    std::vector<std::string> installed_models;

    std::string flm_path = find_flm_binary();
    if (flm_path.empty()) return installed_models;

    std::string output;
#ifdef _WIN32
    std::string command = "\"" + flm_path + "\" list --filter installed --quiet --json 2>NUL";
    int rc = lemon::utils::ProcessManager::run_command(command, output);
#else
    std::string command = "\"" + flm_path + "\" list --filter installed --quiet --json 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return installed_models;
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    pclose(pipe);
#endif

    // Parse output: { "models": [ { "name": "modelname:tag", ... }, ... ] }
    try {
        json j = lemon::utils::JsonUtils::parse(output);
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& model : j["models"]) {
                if (model.contains("name") && model["name"].is_string()) {
                    installed_models.push_back(model["name"].get<std::string>());
                }
            }
            return installed_models;
        }
    } catch (...) {
        // Fallback to legacy parsing if JSON parsing fails
    }

    // Legacy parsing
    // Expected format:
    //   Models:
    //     - modelname:tag
    //     - another:model
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line == "Models:" || line.empty()) {
            continue;
        }

        // Parse model checkpoint (format: "  - modelname:tag")
        if (line.find("- ") == 0) {
            std::string checkpoint = line.substr(2);
            checkpoint.erase(0, checkpoint.find_first_not_of(" \t"));
            checkpoint.erase(checkpoint.find_last_not_of(" \t") + 1);
            if (!checkpoint.empty()) {
                installed_models.push_back(checkpoint);
            }
        }
    }

    return installed_models;
}

std::vector<ModelInfo> flm_discover_models() {
    std::vector<ModelInfo> flm_models;
    if (!SystemInfoCache::get_flm_status().is_ready()) {
        return flm_models;
    }

    std::string flm_path = find_flm_binary();
    if (flm_path.empty()) return flm_models;

    LOG(INFO, "ModelManager") << "FLM binary found at: " << flm_path << std::endl;

    std::string output;
#ifdef _WIN32
    std::string command = "\"" + flm_path + "\" list --json";
    int rc = lemon::utils::ProcessManager::run_command(command, output);
    LOG(INFO, "ModelManager") << "flm list --json exit code: " << rc
              << ", output length: " << output.size() << std::endl;
    if (rc != 0 || output.empty()) {
        LOG(WARNING, "ModelManager") << "flm list --json failed or returned empty. "
                  << "Output: " << output.substr(0, 200) << std::endl;
    }
#else
    std::string command = "\"" + flm_path + "\" list --json 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return flm_models;
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    pclose(pipe);
#endif

    // Parse output: { "models": [ { "name": "modelname:tag", "footprint": 1.23, ... }, ... ] }
    try {
        json j = lemon::utils::JsonUtils::parse(output);
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                if (m.contains("name") && m["name"].is_string()) {
                    std::string checkpoint = m["name"].get<std::string>();

                    // Format display name: replace : with -, append -FLM
                    // e.g., "llama3.2:1b" -> "llama3.2-1b-FLM"
                    std::string display_name = checkpoint;
                    std::replace(display_name.begin(), display_name.end(), ':', '-');

                    std::string model_name = display_name + "-FLM";

                    ModelInfo info;
                    info.model_name = model_name;
                    info.checkpoints["main"] = checkpoint;
                    info.recipe = "flm";
                    info.suggested = true; // All official FLM models are suggested
                    info.downloaded = lemon::utils::JsonUtils::get_or_default<bool>(m, "installed", false);

                    if (lemon::utils::JsonUtils::get_or_default<bool>(m, "installed", false) && m.contains("url") && m["url"].is_string()) {
                        fs::path config_path = backends::fastflowlm::find_flm_config_path_from_repo_dir(
                            backends::fastflowlm::repo_dir_from_url(m["url"].get<std::string>()));
                        if (!config_path.empty()) {
                            info.resolved_paths["config"] = path_to_utf8(config_path);
                        }
                    }

                    // Size in GB (footprint field contains disk size in GB)
                    if (m.contains("footprint") && m["footprint"].is_number()) {
                        info.size = m["footprint"].get<double>();
                    }

                    if (m.contains("label") && m["label"].is_array()) {
                        for (const auto& l : m["label"]) {
                            if (l.is_string()) {
                                info.labels.push_back(l.get<std::string>());
                            }
                        }
                    }

                    info.type = get_model_type_from_labels(info.labels);
                    const BackendDescriptor* flm_desc = descriptor_for("flm");
                    info.device = flm_desc ? flm_desc->default_device : DEVICE_NPU;

                    flm_models.push_back(info);
                }
            }
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "FLM model discovery failed: " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "ModelManager") << "FLM model discovery failed with unknown error" << std::endl;
    }

    return flm_models;
}


void flm_download(const std::string& checkpoint, bool do_not_upgrade,
                  DownloadProgressCallback progress_callback) {
    LOG(INFO, "ModelManager") << "Pulling FLM model: " << checkpoint << std::endl;

    auto status = SystemInfoCache::get_flm_status();
    if (!status.is_ready()) {
        throw std::runtime_error(status.error_string());
    }

    std::string flm_path = find_flm_binary();
    if (flm_path.empty()) {
        throw std::runtime_error("FLM executable not found");
    }

    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }

    LOG(INFO, "ProcessManager") << "Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        LOG(INFO, "ProcessManager") << " \"" << arg << "\"";
    }
    LOG(INFO, "ProcessManager") << std::endl;

    // State for parsing FLM output
    int total_files = 0;
    int current_file_index = 0;
    std::string current_filename;
    bool cancelled = false;

    int exit_code = lemon::utils::ProcessManager::run_process_with_output(
        flm_path, args,
        [&](const std::string& line) -> bool {
            LOG(INFO, "FLM") << line << std::endl;

            // Pattern: "[FLM]  Downloading X/Y: filename"
            if (line.find("[FLM]  Downloading ") != std::string::npos &&
                line.find("/") != std::string::npos &&
                line.find(":") != std::string::npos) {

                // Extract "X/Y: filename" from "[FLM]  Downloading X/Y: filename"
                size_t start = line.find("Downloading ") + 12;
                size_t slash = line.find("/", start);
                size_t colon = line.find(":", slash);

                if (slash != std::string::npos && colon != std::string::npos) {
                    try {
                        current_file_index = std::stoi(line.substr(start, slash - start));
                        total_files = std::stoi(line.substr(slash + 1, colon - slash - 1));
                        current_filename = line.substr(colon + 2);  // Skip ": "

                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = 0;
                            progress.bytes_total = 0;
                            progress.percent = (total_files > 0) ?
                                ((current_file_index - 1) * 100 / total_files) : 0;

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Downloading: XX.X% (XXX.XMB / XXX.XMB)"
            else if (line.find("[FLM]  Downloading: ") != std::string::npos &&
                     line.find("%") != std::string::npos) {

                size_t start = line.find("Downloading: ") + 13;
                size_t pct_end = line.find("%", start);

                if (pct_end != std::string::npos) {
                    try {
                        std::string pct_str = line.substr(start, pct_end - start);
                        double file_percent = std::stod(pct_str);

                        // Try to extract bytes (XXX.XMB / XXX.XMB)
                        size_t open_paren = line.find("(", pct_end);
                        size_t slash = line.find("/", open_paren);
                        size_t close_paren = line.find(")", slash);

                        size_t bytes_downloaded = 0;
                        size_t bytes_total = 0;

                        if (open_paren != std::string::npos && slash != std::string::npos) {
                            std::string downloaded_str = line.substr(open_paren + 1, slash - open_paren - 1);
                            std::string total_str = line.substr(slash + 1, close_paren - slash - 1);

                            // Parse "XXX.XMB" format
                            auto parse_size = [](const std::string& s) -> size_t {
                                double val = 0;
                                size_t mb_pos = s.find("MB");
                                size_t gb_pos = s.find("GB");
                                size_t kb_pos = s.find("KB");

                                if (mb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, mb_pos));
                                    return static_cast<size_t>(val * 1024 * 1024);
                                } else if (gb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, gb_pos));
                                    return static_cast<size_t>(val * 1024 * 1024 * 1024);
                                } else if (kb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, kb_pos));
                                    return static_cast<size_t>(val * 1024);
                                }
                                return 0;
                            };

                            bytes_downloaded = parse_size(downloaded_str);
                            bytes_total = parse_size(total_str);
                        }

                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = bytes_downloaded;
                            progress.bytes_total = bytes_total;
                            // Use intra-file percent when we have byte-level progress
                            progress.percent = static_cast<int>(file_percent);

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Overall progress: XX.X% (X/Y files)"
            else if (line.find("[FLM]  Overall progress: ") != std::string::npos) {
                size_t start = line.find("progress: ") + 10;
                size_t pct_end = line.find("%", start);

                if (pct_end != std::string::npos) {
                    try {
                        int overall_percent = static_cast<int>(std::stod(line.substr(start, pct_end - start)));

                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = 0;  // Not available for overall progress
                            progress.bytes_total = 0;
                            progress.percent = overall_percent;

                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;  // Kill the process
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            // Pattern: "[FLM]  Missing files (N):"
            else if (line.find("[FLM]  Missing files (") != std::string::npos) {
                size_t start = line.find("(") + 1;
                size_t end = line.find(")", start);
                if (end != std::string::npos) {
                    try {
                        total_files = std::stoi(line.substr(start, end - start));
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }

            return true;  // Continue
        },
        "",  // Working directory
        3600  // 1 hour timeout for large model downloads
    );

    if (cancelled) {
        LOG(INFO, "ModelManager") << "FLM download cancelled by client" << std::endl;
        throw std::runtime_error("Download cancelled");
    }

    if (exit_code != 0) {
        LOG(ERROR, "ModelManager") << "FLM pull failed with exit code: " << exit_code << std::endl;
        throw std::runtime_error("FLM pull failed with exit code: " + std::to_string(exit_code));
    }

    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);  // Ignore return - download already complete
    }

    LOG(INFO, "ModelManager") << "FLM model pull completed successfully" << std::endl;
}


std::string flm_version() {
    // Cache real version strings to avoid spawning the subprocess twice per
    // build_recipes_info() pass. "unknown" is NOT cached so that post-install
    // verification in fastflowlm_server.cpp gets a fresh result after FLM is installed.
    static std::string cached_version;
    if (!cached_version.empty()) {
        return cached_version;
    }

    std::string flm_path = find_flm_executable();
    if (flm_path.empty() || !lemon::utils::is_safe_executable_path(flm_path)) {
        return "unknown";
    }

    std::string output;
    #ifdef _WIN32
    std::string command = "\"" + flm_path + "\" version --json 2>NUL";
    int rc = lemon::utils::ProcessManager::run_command(command, output);
    #else
    std::string command = "\"" + flm_path + "\" version --json 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "unknown";
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    pclose(pipe);
    #endif

    // Parse JSON output: { "version": "0.9.34" }
    try {
        json j = lemon::utils::JsonUtils::parse(output);
        if (j.contains("version") && j["version"].is_string()) {
            std::string version = j["version"].get<std::string>();
            // If the version doesn't start with 'v', prepend it
            // for backend_versions.json compatibility (e.g. "v0.9.34").
            if (!version.empty() && version[0] != 'v') {
                version = "v" + version;
            }
            cached_version = version;
            return cached_version;
        }
    } catch (...) {
        // Fallback to legacy parsing if JSON parsing fails
    }

    // Legacy parsing from output like "FLM v0.9.4"
    if (output.find("FLM v") != std::string::npos) {
        size_t pos = output.find("FLM v");
        // Keep the 'v' prefix so it matches backend_versions.json (e.g. "v0.9.34").
        std::string version = output.substr(pos + 4);
        size_t end = version.find_first_of(" \t\n\r");
        if (end != std::string::npos) {
            version = version.substr(0, end);
        }
        cached_version = version;
        return cached_version;
    }

    return "unknown";
}


std::string find_flm_executable() {
#ifdef _WIN32
    // On Windows, only check the Lemonade install directory (auto-installed zip).
    // No system PATH fallback - FLM should be installed via install_backend().
    std::string install_dir = (fs::path(lemon::utils::get_downloaded_bin_dir()) / "flm" / "npu").make_preferred().string();
    if (fs::exists(install_dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(install_dir)) {
            if (entry.is_regular_file() && entry.path().filename().string() == "flm.exe") {
                std::string path = entry.path().string();
                if (lemon::utils::is_safe_executable_path(path)) {
                    return path;
                }
            }
        }
    }
    return "";
#else
    // Walk PATH directly — minimal Fedora/openSUSE containers do not ship `which`.
    if (!lemon::utils::find_executable_in_path("flm").empty()) {
        return "flm";
    }
    return "";
#endif
}

bool run_flm_validate(const std::string& flm_path, std::string& error_message) {
    std::string flm_exe = flm_path.empty() ? find_flm_executable() : flm_path;
    if (flm_exe.empty()) {
        error_message = "FLM executable not found";
        return false;
    }
    if (!lemon::utils::is_safe_executable_path(flm_exe)) {
        error_message = "FLM path contains invalid characters";
        return false;
    }

    std::string command = "\"" + flm_exe + "\" validate --json";
    std::string output;
    int exit_code;
#ifdef _WIN32
    exit_code = lemon::utils::ProcessManager::run_command(command, output);
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        error_message = "Failed to execute " + flm_exe;
        return false;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    exit_code = pclose(pipe);
    if (exit_code != -1) {
        exit_code = WEXITSTATUS(exit_code);
    }
#endif

    try {
        if (!output.empty()) {
            json j = lemon::utils::JsonUtils::parse(output);
            if (j.is_object()) {
                bool validation_ok = false;
                if (j.contains("ready")) {
                    validation_ok = j["ready"].get<bool>();
                }

                if (validation_ok) {
                    error_message.clear();
                    return true;
                }

                std::vector<std::string> errors;

                if (j.contains("amd_device_found") && !j["amd_device_found"].get<bool>()) {
                    errors.push_back("No AMD NPU device found.");
                }

                if (j.contains("all_fw_ok") && !j["all_fw_ok"].get<bool>()) {
                    errors.push_back("NPU firmware is incompatible.");
                }
                if (j.contains("kernel_ok") && !j["kernel_ok"].get<bool>()) {
                    errors.push_back("Kernel version is incompatible.");
                }

                if (j.contains("memlock_ok") && !j["memlock_ok"].get<bool>()) {
                    errors.push_back("Memlock limits are too low.");
                }

                if (j.contains("npu_driver_ok") && !j["npu_driver_ok"].get<bool>()) {
                    errors.push_back("NPU driver version is too old.");
                }

                if (errors.empty()) {
                    error_message = "NPU validation failed.";
                } else {
                    error_message = "";
                    for (size_t i = 0; i < errors.size(); ++i) {
                        error_message += errors[i] + (i == errors.size() - 1 ? "" : " ");
                    }
                }
                return false;
            }
        }
    } catch (...) {
        // Fallback for non-JSON output or parsing error
    }

    if (exit_code != 0) {
        error_message = "flm validate failed with exit code " + std::to_string(exit_code);
        return false;
    }

    error_message.clear();
    return true;
}


void flm_remove(const std::string& checkpoint) {
    if (checkpoint.empty()) {
        throw std::runtime_error("FLM model has empty checkpoint field, cannot delete");
    }
    std::string flm_path = find_flm_binary();
    if (flm_path.empty()) {
        throw std::runtime_error("FLM executable not found");
    }
    std::vector<std::string> args = {"remove", checkpoint};
    auto handle = lemon::utils::ProcessManager::start_process(flm_path, args, "", false);

    int timeout_seconds = 60;
    for (int i = 0; i < timeout_seconds * 10; ++i) {
        if (!lemon::utils::ProcessManager::is_running(handle)) {
            int exit_code = lemon::utils::ProcessManager::get_exit_code(handle);
            if (exit_code != 0) {
                throw std::runtime_error("FLM remove failed for " + checkpoint +
                                         " (exit code " + std::to_string(exit_code) + ")");
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    lemon::utils::ProcessManager::stop_process(handle);
    throw std::runtime_error("FLM remove timed out for " + checkpoint);
}

} // namespace fastflowlm
} // namespace backends
} // namespace lemon
