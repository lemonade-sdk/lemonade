#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/whisper_server.h"
#include "lemon/backends/sd_server.h"
#include "lemon/backends/kokoro_server.h"
#include "lemon/backends/fastflowlm_server.h"
#include "lemon/backends/ryzenaiserver.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include <iostream>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lemon {

BackendManager::BackendManager() {
}

// ============================================================================
// Install parameters for each recipe
// ============================================================================

BackendManager::InstallParams BackendManager::get_llamacpp_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.version = version;

    if (backend == "rocm") {
        params.repo = "lemonade-sdk/llamacpp-rocm";
        std::string target_arch = SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("llamacpp", "rocm")
            );
        }
#ifdef _WIN32
        params.filename = "llama-" + version + "-windows-rocm-" + target_arch + "-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-ubuntu-rocm-" + target_arch + "-x64.zip";
#else
        throw std::runtime_error("ROCm llamacpp only supported on Windows and Linux");
#endif
    } else if (backend == "metal") {
        params.repo = "ggml-org/llama.cpp";
#ifdef __APPLE__
        params.filename = "llama-" + version + "-bin-macos-arm64.tar.gz";
#else
        throw std::runtime_error("Metal llamacpp only supported on macOS");
#endif
    } else if (backend == "cpu") {
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-cpu-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-x64.tar.gz";
#else
        throw std::runtime_error("CPU llamacpp not supported on this platform");
#endif
    } else {  // vulkan
        params.repo = "ggml-org/llama.cpp";
#ifdef _WIN32
        params.filename = "llama-" + version + "-bin-win-vulkan-x64.zip";
#elif defined(__linux__)
        params.filename = "llama-" + version + "-bin-ubuntu-vulkan-x64.tar.gz";
#else
        throw std::runtime_error("Vulkan llamacpp only supported on Windows and Linux");
#endif
    }

    return params;
}

BackendManager::InstallParams BackendManager::get_whispercpp_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.version = version;

    if (backend == "npu") {
        params.repo = "lemonade-sdk/whisper.cpp-npu";
#ifdef _WIN32
        params.filename = "whisper-" + version + "-windows-npu-x64.zip";
#else
        throw std::runtime_error("NPU whisper.cpp only supported on Windows");
#endif
    } else if (backend == "cpu") {
        params.repo = "ggml-org/whisper.cpp";
#ifdef _WIN32
        params.filename = "whisper-bin-x64.zip";
#elif defined(__linux__)
        params.filename = "whisper-bin-x64.zip";
#elif defined(__APPLE__)
        params.filename = "whisper-bin-arm64.zip";
#else
        throw std::runtime_error("Unsupported platform for whisper.cpp");
#endif
    } else {
        throw std::runtime_error("[BackendManager] Unknown whisper backend: " + backend);
    }

    return params;
}

BackendManager::InstallParams BackendManager::get_sdcpp_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "superm1/stable-diffusion.cpp";
    params.version = version;

    // Transform version for URL (master-NNN-HASH -> master-HASH)
    std::string short_version = version;
    size_t first_dash = version.find('-');
    if (first_dash != std::string::npos) {
        size_t second_dash = version.find('-', first_dash + 1);
        if (second_dash != std::string::npos) {
            short_version = version.substr(0, first_dash) + "-" +
                           version.substr(second_dash + 1);
        }
    }

    if (backend == "rocm") {
        std::string target_arch = SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("sd-cpp", "rocm")
            );
        }
#ifdef _WIN32
        params.filename = "sd-" + short_version + "-bin-win-rocm-x64.zip";
#elif defined(__linux__)
        params.filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64-rocm.zip";
#else
        throw std::runtime_error("ROCm sd.cpp only supported on Windows and Linux");
#endif
    } else {
        // CPU build (default)
#ifdef _WIN32
        params.filename = "sd-" + short_version + "-bin-win-avx2-x64.zip";
#elif defined(__linux__)
        params.filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64.zip";
#elif defined(__APPLE__)
        params.filename = "sd-" + short_version + "-bin-Darwin-macOS-15.7.2-arm64.zip";
#else
        throw std::runtime_error("Unsupported platform for stable-diffusion.cpp");
#endif
    }

    return params;
}

BackendManager::InstallParams BackendManager::get_kokoro_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/Kokoros";
    params.version = version;

#ifdef _WIN32
    params.filename = "kokoros-windows-x86_64.tar.gz";
#elif defined(__linux__)
    params.filename = "kokoros-linux-x86_64.tar.gz";
#else
    throw std::runtime_error("Unsupported platform for kokoros");
#endif

    return params;
}

BackendManager::InstallParams BackendManager::get_ryzenai_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/ryzenai-server";
    params.version = version;
    params.filename = "ryzenai-server.zip";
    return params;
}

BackendManager::InstallParams BackendManager::get_install_params(const std::string& recipe, const std::string& backend) {
    std::string version;

    if (recipe == "ryzenai-llm") {
        // ryzenai-server has its version at top level in backend_versions.json
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        json config = utils::JsonUtils::load_from_file(config_path);
        if (config.contains("ryzenai-server") && config["ryzenai-server"].is_string()) {
            version = config["ryzenai-server"].get<std::string>();
        } else if (config.contains("ryzenai-server") && config["ryzenai-server"].is_object()
                   && config["ryzenai-server"].contains("default")) {
            version = config["ryzenai-server"]["default"].get<std::string>();
        } else {
            throw std::runtime_error("backend_versions.json is missing 'ryzenai-server' version");
        }
        return get_ryzenai_params(backend, version);
    }

    if (recipe == "flm") {
        // FLM uses special installer, not install_from_github
        throw std::runtime_error("FLM uses a special installer and cannot be installed via get_install_params");
    }

    // Standard recipes use BackendUtils::get_backend_version
    version = backends::BackendUtils::get_backend_version(recipe, backend);

    if (recipe == "llamacpp") return get_llamacpp_params(backend, version);
    if (recipe == "whispercpp") return get_whispercpp_params(backend, version);
    if (recipe == "sd-cpp") return get_sdcpp_params(backend, version);
    if (recipe == "kokoro") return get_kokoro_params(backend, version);

    throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
}

// ============================================================================
// Core operations
// ============================================================================

static const backends::BackendSpec& get_spec_for_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") return backends::LlamaCppServer::SPEC;
    if (recipe == "whispercpp") return backends::WhisperServer::SPEC;
    if (recipe == "sd-cpp") return backends::SDServer::SPEC;
    if (recipe == "kokoro") return backends::KokoroServer::SPEC;

    // ryzenai-llm uses a custom spec since it doesn't have a standard SPEC
    static const backends::BackendSpec ryzenai_spec("ryzenai-server",
#ifdef _WIN32
        "ryzenai-server.exe"
#else
        "ryzenai-server"
#endif
    );
    if (recipe == "ryzenai-llm") return ryzenai_spec;

    throw std::runtime_error("[BackendManager] Unknown recipe: " + recipe);
}

void BackendManager::install_backend(const std::string& recipe, const std::string& backend,
                                     DownloadProgressCallback progress_cb) {
    std::cout << "[BackendManager] Installing " << recipe << ":" << backend << std::endl;

    // FLM special case - uses installer exe
    if (recipe == "flm") {
        install_flm(backend);
        if (progress_cb) {
            DownloadProgress p;
            p.file = "flm-setup.exe";
            p.file_index = 1;
            p.total_files = 1;
            p.percent = 100;
            p.complete = true;
            progress_cb(p);
        }
        return;
    }

    auto params = get_install_params(recipe, backend);
    const auto& spec = get_spec_for_recipe(recipe);

    // For ryzenai-llm, the backend field in install_from_github is "default" (no backend variants)
    std::string backend_dir = (recipe == "ryzenai-llm") ? "" : backend;

    backends::BackendUtils::install_from_github(
        spec, params.version, params.repo, params.filename, backend_dir, progress_cb);
}

void BackendManager::install_flm(const std::string& backend) {
    // FLM install is complex (driver checks, installer exe, etc.)
    // We delegate to FastFlowLMServer's static install logic
    // For now, create a temporary FastFlowLMServer just to call install()
    // This is a stopgap - ideally we'd extract the FLM install logic to a shared place
    backends::FastFlowLMServer flm_server("info", nullptr);
    flm_server.install(backend);
}

void BackendManager::uninstall_backend(const std::string& recipe, const std::string& backend) {
    std::cout << "[BackendManager] Uninstalling " << recipe << ":" << backend << std::endl;

    if (recipe == "flm") {
        throw std::runtime_error("FLM cannot be uninstalled via Backend Manager (system installation)");
    }

    std::string dir_name;
    std::string backend_dir;

    if (recipe == "ryzenai-llm") {
        dir_name = "ryzenai-server";
        backend_dir = "";
    } else {
        const auto& spec = get_spec_for_recipe(recipe);
        dir_name = spec.recipe;
        backend_dir = backend;
    }

    std::string install_dir = backends::BackendUtils::get_install_directory(dir_name, backend_dir);

    if (fs::exists(install_dir)) {
        fs::remove_all(install_dir);
        std::cout << "[BackendManager] Removed: " << install_dir << std::endl;
    } else {
        std::cout << "[BackendManager] Nothing to uninstall at: " << install_dir << std::endl;
    }
}

// ============================================================================
// Query operations
// ============================================================================

bool BackendManager::is_installed(const std::string& recipe, const std::string& backend) {
    if (recipe == "ryzenai-llm") {
        return RyzenAIServer::is_available();
    }

    if (recipe == "flm") {
        return SystemInfo::get_flm_version() != "";
    }

    try {
        const auto& spec = get_spec_for_recipe(recipe);
        std::string path = backends::BackendUtils::get_backend_binary_path(spec, backend);
        return !path.empty();
    } catch (...) {
        return false;
    }
}

std::string BackendManager::get_installed_version(const std::string& recipe, const std::string& backend) {
    if (recipe == "ryzenai-llm") {
        return SystemInfo::get_oga_version();
    }
    if (recipe == "flm") {
        return SystemInfo::get_flm_version();
    }

    try {
        const auto& spec = get_spec_for_recipe(recipe);
        std::string version_file = backends::BackendUtils::get_installed_version_file(spec, backend);
        if (fs::exists(version_file)) {
            std::ifstream vf(version_file);
            std::string version;
            std::getline(vf, version);
            return version;
        }
    } catch (...) {}

    return "";
}

std::string BackendManager::get_latest_version(const std::string& recipe, const std::string& backend) {
    try {
        if (recipe == "ryzenai-llm") {
            std::string config_path = utils::get_resource_path("resources/backend_versions.json");
            json config = utils::JsonUtils::load_from_file(config_path);
            if (config.contains("ryzenai-server") && config["ryzenai-server"].is_string()) {
                return config["ryzenai-server"].get<std::string>();
            }
            return "";
        }

        if (recipe == "flm") {
            std::string config_path = utils::get_resource_path("resources/backend_versions.json");
            json config = utils::JsonUtils::load_from_file(config_path);
            if (config.contains("flm") && config["flm"].contains("version")) {
                return config["flm"]["version"].get<std::string>();
            }
            return "";
        }

        return backends::BackendUtils::get_backend_version(recipe, backend);
    } catch (...) {
        return "";
    }
}

json BackendManager::get_all_backends_status() {
    auto statuses = SystemInfo::get_all_recipe_statuses();
    json result = json::array();

    for (const auto& recipe : statuses) {
        json recipe_json;
        recipe_json["recipe"] = recipe.name;
        recipe_json["supported"] = recipe.supported;
        recipe_json["available"] = recipe.available;
        if (!recipe.error.empty()) {
            recipe_json["error"] = recipe.error;
        }

        json backends_json = json::array();
        for (const auto& backend : recipe.backends) {
            json b;
            b["name"] = backend.name;
            b["supported"] = backend.supported;
            b["available"] = backend.available;
            if (!backend.version.empty()) {
                b["version"] = backend.version;
            }
            if (!backend.error.empty()) {
                b["error"] = backend.error;
            }

            // Add release URL
            std::string release_url = get_release_url(recipe.name, backend.name);
            if (!release_url.empty()) {
                b["release_url"] = release_url;
            }

            backends_json.push_back(b);
        }
        recipe_json["backends"] = backends_json;
        result.push_back(recipe_json);
    }

    return result;
}

std::string BackendManager::get_release_url(const std::string& recipe, const std::string& backend) {
    try {
        if (recipe == "flm") {
            std::string version = get_latest_version(recipe, backend);
            if (!version.empty()) {
                return "https://github.com/FastFlowLM/FastFlowLM/releases/tag/" + version;
            }
            return "";
        }

        if (recipe == "ryzenai-llm") {
            std::string version = get_latest_version(recipe, backend);
            if (!version.empty()) {
                return "https://github.com/lemonade-sdk/ryzenai-server/releases/tag/" + version;
            }
            return "";
        }

        auto params = get_install_params(recipe, backend);
        return "https://github.com/" + params.repo + "/releases/tag/" + params.version;
    } catch (...) {
        return "";
    }
}

std::string BackendManager::get_download_filename(const std::string& recipe, const std::string& backend) {
    try {
        auto params = get_install_params(recipe, backend);
        return params.filename;
    } catch (...) {
        return "";
    }
}

} // namespace lemon
