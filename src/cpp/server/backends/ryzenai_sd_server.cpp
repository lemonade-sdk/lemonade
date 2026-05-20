#include "lemon/backends/ryzenai_sd_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

InstallParams SDNPUServer::get_install_params(const std::string& /* backend */, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/ryzenai-sd-server";
#ifdef _WIN32
    params.filename = "ryzenai-sd-server-" + version + "-win-x64.zip";
#elif defined(__linux__)
    params.filename = "ryzenai-sd-server-" + version + "-Linux-x86_64.zip";
#else
    throw std::runtime_error("SD NPU server is only supported on Windows and Linux");
#endif
    return params;
}

SDNPUServer::SDNPUServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("sd-npu-server", log_level, model_manager, backend_manager) {
    LOG(DEBUG, "SDNPUServer") << "Created with log_level=" << log_level << std::endl;
}

SDNPUServer::~SDNPUServer() {
    unload();
}

void SDNPUServer::load(const std::string& model_name,
                       const ModelInfo& model_info,
                       const RecipeOptions& options,
                       bool /* do_not_upgrade */) {
    LOG(INFO, "SDNPUServer") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "SDNPUServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    image_defaults_ = model_info.image_defaults;

    // Determine backend (npu or cpu)
    std::string backend = options.get_option("sd-npu_backend");
    if (backend.empty()) {
        backend = "npu";
    }
    bool use_cpu = (backend == "cpu");

    device_type_ = use_cpu ? DEVICE_CPU : DEVICE_NPU;

    // Get model path - NPU models are directories with ONNX files
    std::string model_path = model_info.resolved_path("main");

    if (model_path.empty()) {
        throw std::runtime_error("Model path not found for: " + model_info.checkpoint());
    }

    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model path does not exist: " + model_path);
    }

    LOG(DEBUG, "SDNPUServer") << "Using model: " << model_path << std::endl;

    // Install sd-npu-server if needed, then resolve the binary path
    backend_manager_->install_backend(SPEC.recipe, backend);
    std::string ryzenai_sd_server_path = BackendUtils::get_backend_binary_path(SPEC, backend);

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "SDNPUServer") << "Starting server on port " << port_
                            << " (backend: " << backend << ")" << std::endl;

    // Resolve DD (DynamicDispatch) paths for NPU model loading.
    // Priority: RyzenAI GenAI-SD install > RyzenAI deployment > empty (let subprocess default)
    // Skipped when running in CPU mode.
    std::string dd_root;
    std::string dd_plugins;
#ifdef _WIN32
    if (!use_cpu) {
        // Check RyzenAI 1.7.1 GenAI-SD paths first
        fs::path genai_sd_lib = "C:/Program Files/RyzenAI/1.7.1/GenAI-SD/lib";
        fs::path genai_sd_stx = genai_sd_lib / "transaction" / "stx";
        fs::path deploy_lib = "C:/Program Files/RyzenAI/1.7.1/deployment";
        fs::path deploy_stx = deploy_lib / "transaction" / "stx";

        if (fs::exists(genai_sd_stx)) {
            dd_root = genai_sd_lib.string();
            dd_plugins = genai_sd_stx.string();
        } else if (fs::exists(deploy_stx)) {
            dd_root = deploy_lib.string();
            dd_plugins = deploy_stx.string();
        }
    }
#endif

    // Build command line arguments
    std::vector<std::string> args = {
        "--server",
        "--port", std::to_string(port_),
        "--model-path", model_path
    };

    // Force CPU execution provider if requested
    if (use_cpu) {
        args.push_back("--force-cpu");
    }

    // Pass DD paths as CLI args so ryzenai-sd-server uses them
    if (!dd_root.empty()) {
        args.push_back("--dd-root");
        args.push_back(dd_root);
        args.push_back("--dd-plugins-root");
        args.push_back(dd_plugins);
    }

    // Also set DD env vars for the subprocess
    std::vector<std::pair<std::string, std::string>> env_vars;
    if (!dd_root.empty()) {
        env_vars.push_back({"DD_ROOT", dd_root});
        env_vars.push_back({"DD_PLUGINS_ROOT", dd_plugins});
        LOG(INFO, "SDNPUServer") << "DD_ROOT=" << dd_root << std::endl;
        LOG(INFO, "SDNPUServer") << "DD_PLUGINS_ROOT=" << dd_plugins << std::endl;
    }

    // Launch the server process
    process_handle_ = utils::ProcessManager::start_process(
        ryzenai_sd_server_path,
        args,
        "",         // working_dir (empty = current)
        is_debug(), // inherit_output
        true,       // filter_health_logs (suppress noisy health-check output)
        env_vars
    );

    if (process_handle_.pid == 0) {
        throw std::runtime_error("Failed to start ryzenai-sd-server process");
    }

    LOG(INFO, "SDNPUServer") << "Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("ryzenai-sd-server failed to start or become ready");
    }

    LOG(INFO, "SDNPUServer") << "NPU server is ready at http://127.0.0.1:" << port_ << std::endl;
}

void SDNPUServer::unload() {
    if (process_handle_.pid != 0) {
        LOG(INFO, "SDNPUServer") << "Stopping NPU server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
}

// ICompletionServer implementation - not supported for image generation
json SDNPUServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "sd-npu (image generation model)")
    );
}

json SDNPUServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "sd-npu (image generation model)")
    );
}

json SDNPUServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "sd-npu (image generation model)")
    );
}

std::string SDNPUServer::resolve_size(const json& request) const {
    if (request.contains("size") && request["size"].is_string()) {
        return request["size"].get<std::string>();
    }
    if (request.contains("width") && request.contains("height") &&
        request["width"].is_number_integer() && request["height"].is_number_integer()) {
        return std::to_string(request["width"].get<int>()) + "x"
             + std::to_string(request["height"].get<int>());
    }
    if (image_defaults_.has_defaults) {
        return std::to_string(image_defaults_.width) + "x"
             + std::to_string(image_defaults_.height);
    }
    return "";
}

json SDNPUServer::image_generations(const json& request) {
    json sd_request = request;

    // Apply size from request, image_defaults, or leave absent
    std::string size = resolve_size(request);
    if (!size.empty()) {
        sd_request["size"] = size;
    }

    // Build extra args for steps / cfg_scale / seed, using image_defaults as fallback
    json extra_args;
    if (request.contains("steps") && request["steps"].is_number_integer()) {
        extra_args["steps"] = request["steps"].get<int>();
    } else if (image_defaults_.has_defaults) {
        extra_args["steps"] = image_defaults_.steps;
    } else {
        json steps_opt = recipe_options_.get_option("steps");
        if (!steps_opt.is_null()) {
            extra_args["steps"] = static_cast<int>(steps_opt);
        }
    }
    if (request.contains("cfg_scale") && request["cfg_scale"].is_number()) {
        extra_args["cfg_scale"] = request["cfg_scale"].get<float>();
    } else if (image_defaults_.has_defaults) {
        extra_args["cfg_scale"] = image_defaults_.cfg_scale;
    } else {
        json cfg_opt = recipe_options_.get_option("cfg_scale");
        if (!cfg_opt.is_null()) {
            extra_args["cfg_scale"] = static_cast<float>(cfg_opt);
        }
    }
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        extra_args["seed"] = request["seed"].get<int>();
    }

    // Append extra args to prompt if any were set
    if (!extra_args.empty()) {
        std::string prompt = sd_request.value("prompt", "");
        prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
        sd_request["prompt"] = prompt;
    }

    LOG(DEBUG, "SDNPUServer") << "Forwarding request to ryzenai-sd-server: "
                              << sd_request.dump(2) << std::endl;

    // ryzenai-sd-server uses OpenAI-compatible /v1/images/generations endpoint
    // Use extended timeout for image generation (can take several seconds on NPU)
    return forward_request("/v1/images/generations", sd_request, utils::HttpClient::get_default_timeout());
}

json SDNPUServer::image_edits(const json& request) {
    // Build extra args for steps / cfg_scale / seed, using image_defaults as fallback
    json extra_args;
    if (request.contains("steps") && request["steps"].is_number_integer()) {
        extra_args["steps"] = request["steps"].get<int>();
    } else if (image_defaults_.has_defaults) {
        extra_args["steps"] = image_defaults_.steps;
    }
    if (request.contains("cfg_scale") && request["cfg_scale"].is_number()) {
        extra_args["cfg_scale"] = request["cfg_scale"].get<float>();
    } else if (image_defaults_.has_defaults) {
        extra_args["cfg_scale"] = image_defaults_.cfg_scale;
    }
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        extra_args["seed"] = request["seed"].get<int>();
    }

    std::string prompt = request.value("prompt", "");
    if (!extra_args.empty()) {
        prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    }

    std::vector<MultipartField> fields;
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    if (request.contains("image_data")) {
        std::string image_binary = JsonUtils::base64_decode(
            request["image_data"].get<std::string>());
        fields.push_back({"image[]", image_binary, "image.bin", "application/octet-stream"});
    }
    if (request.contains("mask_data")) {
        std::string mask_binary = JsonUtils::base64_decode(
            request["mask_data"].get<std::string>());
        fields.push_back({"mask", mask_binary, "mask.bin", "application/octet-stream"});
    }

    LOG(DEBUG, "SDNPUServer") << "Forwarding image edits to /v1/images/edits (multipart)" << std::endl;

    return forward_multipart_request("/v1/images/edits", fields, utils::HttpClient::get_default_timeout());
}

json SDNPUServer::image_variations(const json& request) {
    json extra_args;
    if (request.contains("steps") && request["steps"].is_number_integer()) {
        extra_args["steps"] = request["steps"].get<int>();
    } else if (image_defaults_.has_defaults) {
        extra_args["steps"] = image_defaults_.steps;
    }
    if (request.contains("cfg_scale") && request["cfg_scale"].is_number()) {
        extra_args["cfg_scale"] = request["cfg_scale"].get<float>();
    } else if (image_defaults_.has_defaults) {
        extra_args["cfg_scale"] = image_defaults_.cfg_scale;
    }
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        extra_args["seed"] = request["seed"].get<int>();
    }
    if (request.contains("strength") && request["strength"].is_number()) {
        extra_args["strength"] = request["strength"].get<float>();
    }

    std::string prompt = request.value("prompt", "variation");
    if (!extra_args.empty()) {
        prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    }

    std::vector<MultipartField> fields;
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    // Forward strength as a form field for img2img
    if (request.contains("strength") && request["strength"].is_number()) {
        fields.push_back({"strength", std::to_string(request["strength"].get<float>()), "", ""});
    }

    if (request.contains("image_data")) {
        std::string image_binary = JsonUtils::base64_decode(
            request["image_data"].get<std::string>());
        fields.push_back({"image[]", image_binary, "image.bin", "application/octet-stream"});
    }

    LOG(DEBUG, "SDNPUServer") << "Forwarding image variations to /v1/images/variations (multipart)" << std::endl;

    return forward_multipart_request("/v1/images/variations", fields, utils::HttpClient::get_default_timeout());
}

} // namespace backends
} // namespace lemon
