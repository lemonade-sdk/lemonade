#include "lemon/backends/sd_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <httplib.h>
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

SDServer::SDServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("sd-server", log_level, model_manager) {
    if (is_debug()) {
        std::cout << "[SDServer] Created with log_level=" << log_level << std::endl;
    }
}

SDServer::~SDServer() {
    unload();
}

void SDServer::install(const std::string& backend) {
    std::string repo = "superm1/stable-diffusion.cpp";
    std::string filename;
    std::string expected_version = BackendUtils::get_backend_version(SPEC.recipe, backend);

    // Transform version for URL (master-NNN-HASH -> master-HASH)
    std::string short_version = expected_version;
    size_t first_dash = expected_version.find('-');
    if (first_dash != std::string::npos) {
        size_t second_dash = expected_version.find('-', first_dash + 1);
        if (second_dash != std::string::npos) {
            short_version = expected_version.substr(0, first_dash) + "-" +
                           expected_version.substr(second_dash + 1);
        }
    }

    // ROCm backend selection for AMD GPU support
    if (backend == "rocm") {
        // Validate ROCm architecture support
        std::string target_arch = lemon::SystemInfo::get_rocm_arch();
        if (target_arch.empty()) {
            throw std::runtime_error(
                lemon::SystemInfo::get_unsupported_backend_error("sd-cpp", "rocm")
            );
        }

#ifdef _WIN32
        filename = "sd-" + short_version + "-bin-win-rocm-x64.zip";
#elif defined(__linux__)
        filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64-rocm.zip";
#else
        throw std::runtime_error("ROCm sd.cpp only supported on Windows and Linux");
#endif
        std::cout << "[SDServer] Using ROCm GPU backend" << std::endl;
    } else {
        // CPU build (default)
#ifdef _WIN32
        filename = "sd-" + short_version + "-bin-win-avx2-x64.zip";
#elif defined(__linux__)
        filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64.zip";
#elif defined(__APPLE__)
        filename = "sd-" + short_version + "-bin-Darwin-macOS-15.7.2-arm64.zip";
#else
        throw std::runtime_error("Unsupported platform for stable-diffusion.cpp");
#endif
    }

    BackendUtils::install_from_github(SPEC, expected_version, repo, filename, backend);
}

void SDServer::load(const std::string& model_name,
                    const ModelInfo& model_info,
                    const RecipeOptions& options,
                    bool /* do_not_upgrade */) {
    std::cout << "[SDServer] Loading model: " << model_name << std::endl;
    std::cout << "[SDServer] Per-model settings: " << options.to_log_string() << std::endl;

    std::string backend = options.get_option("sd-cpp_backend");

    // Install sd-server if needed
    install(backend);

    // Get model path
    std::string model_path = model_info.resolved_path("main");
    std::string llm_path = model_info.resolved_path("text_encoder");
    std::string vae_path = model_info.resolved_path("vae");

    if (model_path.empty()) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }

    if (fs::is_directory(model_path)) {
        throw std::runtime_error("Model path is a directory, not a file: " + model_path);
    }

    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model file does not exist: " + model_path);
    }

    std::cout << "[SDServer] Using model: " << model_path << std::endl;

    // Get sd-server executable path
    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, backend);

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::cout << "[SDServer] Starting server on port " << port_ << " (backend: " << backend << ")" << std::endl;

    // Build command line arguments
    std::vector<std::string> args = {
        "--listen-port", std::to_string(port_)
    };

    if (llm_path.empty() || vae_path.empty()) {
        args.push_back("-m");
        args.push_back(model_path);
    } else {
        args.push_back("--diffusion-model");
        args.push_back(model_path);
        args.push_back("--llm");
        args.push_back(llm_path);
        args.push_back("--vae");
        args.push_back(vae_path);
    }

    if (is_debug()) {
        args.push_back("-v");
    }

    // Set up environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();

#ifndef _WIN32
    // For Linux, always set LD_LIBRARY_PATH to include executable directory
    std::string lib_path = exe_dir.string();

    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    if (is_debug()) {
        std::cout << "[SDServer] Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
    }
#else
    // ROCm builds on Windows require hipblaslt.dll, rocblas.dll, amdhip64.dll, etc.
    // These DLLs are distributed alongside sd-server.exe but need PATH to be set for loading
    if (backend == "rocm") {
        // Add executable directory to PATH for ROCm runtime DLLs
        // This allows the sd-server.exe to find required HIP/ROCm libraries at runtime
        std::string new_path = exe_dir.string();
        const char* existing_path = std::getenv("PATH");
        if (existing_path && strlen(existing_path) > 0) {
            new_path = new_path + ";" + std::string(existing_path);
        }
        env_vars.push_back({"PATH", new_path});

        std::cout << "[SDServer] ROCm backend: added " << exe_dir.string() << " to PATH" << std::endl;
    }
#endif

    // Launch the server process
    process_handle_ = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug(),  // inherit_output
        false,  // filter_health_logs
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start sd-server process");
    }

    std::cout << "[SDServer] Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/")) {
        unload();
        throw std::runtime_error("sd-server failed to start or become ready");
    }

    std::cout << "[SDServer] Server is ready at http://127.0.0.1:" << port_ << std::endl;
}

void SDServer::unload() {
    if (process_handle_.pid != 0) {
        std::cout << "[SDServer] Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
}

// ICompletionServer implementation - not supported for image generation
json SDServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "sd-cpp (image generation model)")
    );
}

json SDServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "sd-cpp (image generation model)")
    );
}

json SDServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "sd-cpp (image generation model)")
    );
}

json SDServer::image_generations(const json& request) {
    // Build request - sd-server uses OpenAI-compatible format
    json sd_request = request;

    // sd-server requires extra params (steps, sample_method, scheduler) to be
    // embedded in the prompt as <sd_cpp_extra_args>JSON</sd_cpp_extra_args>
    // See PR #1173: https://github.com/leejet/stable-diffusion.cpp/pull/1173
    // Always inject model defaults from recipe_options so sd-server uses the
    // correct values even when clients send OpenAI-compliant requests without
    // these backend-specific parameters.
    json extra_args;
    extra_args["steps"] = static_cast<int>(recipe_options_.get_option("steps"));
    extra_args["cfg_scale"] = static_cast<float>(recipe_options_.get_option("cfg_scale"));

    // Append extra args to prompt
    {
        std::string prompt = sd_request.value("prompt", "");
        prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
        sd_request["prompt"] = prompt;
    }

    if (is_debug()) {
        std::cout << "[SDServer] Forwarding request to sd-server: "
                  << sd_request.dump(2) << std::endl;
    }

    // Use base class forward_request with 10 minute timeout for image generation
    return forward_request("/v1/images/generations", sd_request, 600);
}

json SDServer::image_edits(const json& request) {
    // sd-server's /v1/images/edits puts images into ref_images (EDIT mode),
    // which does NOT support img2img strength or mask-based inpainting.
    // Instead, use /sdapi/v1/img2img which properly sets init_image and
    // triggers the IMG2IMG code path with strength and mask support.
    json sd_request;

    sd_request["prompt"] = request.value("prompt", "");

    // Image and mask as base64 strings for the JSON body
    if (request.contains("image_data")) {
        sd_request["init_images"] = json::array({request["image_data"]});
    }
    if (request.contains("mask_data")) {
        sd_request["mask"] = request["mask_data"];
    }

    // Parse size string (e.g. "1024x1024") into width/height
    int width = 1024;
    int height = 1024;
    if (request.contains("size") && request["size"].is_string()) {
        std::string size_str = request["size"];
        auto x_pos = size_str.find('x');
        if (x_pos != std::string::npos) {
            width = std::stoi(size_str.substr(0, x_pos));
            height = std::stoi(size_str.substr(x_pos + 1));
        }
    }
    sd_request["width"] = width;
    sd_request["height"] = height;

    // Map n to batch_size (default 1)
    sd_request["batch_size"] = request.value("n", 1);

    // Inject model defaults for sd-server parameters not exposed in OpenAI API
    int steps = recipe_options_.get_option("steps");
    float cfg_scale = recipe_options_.get_option("cfg_scale");
    sd_request["steps"] = steps;
    sd_request["cfg_scale"] = cfg_scale;
    sd_request["denoising_strength"] = 0.75;

    if (is_debug()) {
        // Log without the image data to avoid flooding
        json debug_log = sd_request;
        if (debug_log.contains("init_images")) debug_log["init_images"] = "[base64 image data]";
        if (debug_log.contains("mask")) debug_log["mask"] = "[base64 mask data]";
        std::cout << "[SDServer] Forwarding image edits to /sdapi/v1/img2img: "
                  << debug_log.dump(2) << std::endl;
    }

    // Use the img2img endpoint which properly handles init_image + mask + strength
    auto response = forward_request("/sdapi/v1/img2img", sd_request, 600);

    // Transform sdapi response format {"images": ["b64..."]}
    // to OpenAI format {"data": [{"b64_json": "..."}]}
    if (response.contains("images") && response["images"].is_array()) {
        json openai_response;
        openai_response["created"] = static_cast<long long>(std::time(nullptr));
        openai_response["data"] = json::array();
        for (const auto& img : response["images"]) {
            json item;
            item["b64_json"] = img;
            openai_response["data"].push_back(item);
        }
        return openai_response;
    }

    return response;
}

json SDServer::image_variations(const json& request) {
    // Image variations uses img2img with high denoising strength
    // to generate variations of the input image.
    // Note: OpenAI variations API does NOT accept a prompt parameter.
    json sd_request;

    // Use empty prompt for variations (OpenAI API doesn't support prompt here)
    sd_request["prompt"] = "";

    // Image as base64 string for the JSON body
    if (request.contains("image_data")) {
        sd_request["init_images"] = json::array({request["image_data"]});
    }

    // Parse size string (e.g. "1024x1024") into width/height
    int width = 1024;
    int height = 1024;
    if (request.contains("size") && request["size"].is_string()) {
        std::string size_str = request["size"];
        auto x_pos = size_str.find('x');
        if (x_pos != std::string::npos) {
            width = std::stoi(size_str.substr(0, x_pos));
            height = std::stoi(size_str.substr(x_pos + 1));
        }
    }
    sd_request["width"] = width;
    sd_request["height"] = height;

    // Map n to batch_size (default 1)
    sd_request["batch_size"] = request.value("n", 1);

    // Inject model defaults for sd-server parameters not exposed in OpenAI API
    int steps = recipe_options_.get_option("steps");
    float cfg_scale = recipe_options_.get_option("cfg_scale");
    sd_request["steps"] = steps;
    sd_request["cfg_scale"] = cfg_scale;

    // Use high denoising strength for variations (0.7-0.9 gives good variation)
    sd_request["denoising_strength"] = 0.75;

    if (is_debug()) {
        // Log without the image data to avoid flooding
        json debug_log = sd_request;
        if (debug_log.contains("init_images")) debug_log["init_images"] = "[base64 image data]";
        std::cout << "[SDServer] Forwarding image variations to /sdapi/v1/img2img: "
                  << debug_log.dump(2) << std::endl;
    }

    // Use the img2img endpoint with high denoising for variations
    auto response = forward_request("/sdapi/v1/img2img", sd_request, 600);

    // Transform sdapi response format {"images": ["b64..."]}
    // to OpenAI format {"data": [{"b64_json": "..."}]}
    if (response.contains("images") && response["images"].is_array()) {
        json openai_response;
        openai_response["created"] = static_cast<long long>(std::time(nullptr));
        openai_response["data"] = json::array();
        for (const auto& img : response["images"]) {
            json item;
            item["b64_json"] = img;
            openai_response["data"].push_back(item);
        }
        return openai_response;
    }

    return response;
}

} // namespace backends
} // namespace lemon
