#include "lemon/backends/sd_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <httplib.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <set>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

static const char* ROCM_STABLE_RUNTIME_DIR = "rocm-stable-runtime";

namespace {
bool is_rocm_backend(const std::string& backend) {
    return backend == "rocm" || backend == "rocm-stable" || backend == "rocm-preview";
}

std::string resolve_sdcpp_backend(const std::string& backend) {
    if (backend == "rocm") {
        std::string channel = "preview";
        if (auto* cfg = RuntimeConfig::global()) {
            channel = cfg->rocm_channel();
        }
        // sd.cpp has no nightly build - fall back to preview
        if (channel == "nightly") {
            channel = "preview";
        }
        return "rocm-" + channel;
    }
    return backend;
}

std::string trim_version_prefix(const std::string& version) {
    if (!version.empty() && version[0] == 'v') {
        return version.substr(1);
    }
    return version;
}

std::string trim_to_major_minor(const std::string& version) {
    // Trim to MAJOR.MINOR format (e.g., "7.12.0" -> "7.12")
    std::string trimmed = trim_version_prefix(version);
    size_t second_dot = trimmed.find('.', trimmed.find('.') + 1);
    if (second_dot != std::string::npos) {
        return trimmed.substr(0, second_dot);
    }
    return trimmed;
}

std::string get_rocm_stable_runtime_version() {
    auto config = JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));

    if (config.contains("rocm-stable-runtime") && config["rocm-stable-runtime"].is_string()) {
        return trim_version_prefix(config["rocm-stable-runtime"].get<std::string>());
    }

    throw std::runtime_error("backend_versions.json is missing 'rocm-stable-runtime'");
}

std::string get_therock_version() {
    auto config = JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
    if (!config.contains("therock") || !config["therock"].is_object() ||
        !config["therock"].contains("version") || !config["therock"]["version"].is_string()) {
        throw std::runtime_error("backend_versions.json is missing 'therock.version'");
    }
    // stable-diffusion.cpp release assets include full ROCm runtime version in filenames
    // (for example: rocm-7.12.0), so keep the patch component.
    return trim_version_prefix(config["therock"]["version"].get<std::string>());
}
}

InstallParams SDServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/stable-diffusion.cpp";
    std::string resolved_backend = resolve_sdcpp_backend(backend);

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

    if (is_rocm_backend(resolved_backend)) {
        std::string target_arch = SystemInfo::get_rocm_arch();

        if (target_arch.empty()) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error("sd-cpp", "rocm")
            );
        }
#ifdef _WIN32
        if (resolved_backend == "rocm-stable") {
            // Windows stable diffusion artifacts are currently built against ROCm 7.1.1
            // and don't follow the rocm-stable-runtime version from backend_versions.json
            params.filename = "sd-" + short_version + "-bin-win-rocm-7.1.1-x64.zip";
        } else {  // rocm-preview
            params.filename = "sd-" + short_version + "-bin-win-rocm-" + get_therock_version() + "-x64.zip";
        }
#elif defined(__linux__)
    if (resolved_backend == "rocm-stable") {
        params.filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64-rocm-" +
                  get_rocm_stable_runtime_version() + ".zip";
    } else {
        params.filename = "sd-" + short_version + "-bin-Linux-Ubuntu-24.04-x86_64-rocm-" +
                  get_therock_version() + ".zip";
    }
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

SDServer::SDServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("sd-server", log_level, model_manager, backend_manager) {
    LOG(DEBUG, "SDServer") << "Created with log_level=" << log_level << std::endl;
}

SDServer::~SDServer() {
    unload();
}

void SDServer::load(const std::string& model_name,
                    const ModelInfo& model_info,
                    const RecipeOptions& options,
                    bool /* do_not_upgrade */) {
    LOG(INFO, "SDServer") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "SDServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    image_defaults_ = model_info.image_defaults;

    std::string backend = options.get_option("sd-cpp_backend");
    std::string resolved_backend = resolve_sdcpp_backend(backend);
    std::string sdcpp_args = options.get_option("sdcpp_args");

    RuntimeConfig::validate_backend_choice("sdcpp", backend);

    // Update device type based on the actual backend selected.
    // get_device_type_from_recipe() defaults sd-cpp to CPU, but rocm/vulkan are GPU backends.
    if (backend == "rocm" || backend == "vulkan") {
        device_type_ = DEVICE_GPU;
    } else {
        device_type_ = DEVICE_CPU;
    }

    // Install sd-server if needed
    backend_manager_->install_backend(SPEC.recipe, backend);

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

    LOG(DEBUG, "SDServer") << "Using model: " << model_path << std::endl;

    // Get sd-server executable path
    std::string exe_path = BackendUtils::get_backend_binary_path(SPEC, backend);

    // Choose a port
    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "SDServer") << "Starting server on port " << port_ << " (backend: " << backend << ")" << std::endl;

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

    std::set<std::string> reserved_flags = {
        "-m",
        "--model",
        "--diffusion-model",
        "--llm",
        "--vae",
        "-v",
        "--listen-port"
    };

    if (!sdcpp_args.empty()) {
        std::string validation_error = validate_custom_args(sdcpp_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom sd-server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "SDServer") << "Adding custom arguments: " << sdcpp_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(sdcpp_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    // Set up environment variables
    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();

#ifndef _WIN32
    // For Linux, always set LD_LIBRARY_PATH to include executable directory
    std::string lib_path = exe_dir.string();

    if (resolved_backend == "rocm-stable") {
        std::string runtime_dir = BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
        if (fs::exists(runtime_dir)) {
            lib_path = runtime_dir + ":" + lib_path;
        }
    } else if (resolved_backend == "rocm-preview") {
        std::string rocm_arch = SystemInfo::get_rocm_arch();
        if (!rocm_arch.empty()) {
            std::string therock_lib = BackendUtils::get_therock_lib_path(rocm_arch);
            if (!therock_lib.empty()) {
                lib_path = therock_lib + ":" + lib_path;
            }
        }
    }

    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    LOG(DEBUG, "SDServer") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
#else
    // ROCm builds on Windows require hipblaslt.dll, rocblas.dll, amdhip64.dll, etc.
    // These DLLs are distributed alongside sd-server.exe but need PATH to be set for loading
    if (is_rocm_backend(resolved_backend)) {
        // Add executable directory to PATH for ROCm runtime DLLs
        // This allows the sd-server.exe to find required HIP/ROCm libraries at runtime
        std::string new_path = exe_dir.string();

        if (resolved_backend == "rocm-stable") {
            std::string runtime_dir = BackendUtils::get_install_directory(ROCM_STABLE_RUNTIME_DIR, "");
            if (fs::exists(runtime_dir)) {
                new_path = runtime_dir + ";" + new_path;
            }
        } else if (resolved_backend == "rocm-preview") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_bin = BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_bin.empty()) {
                    new_path = therock_bin + ";" + new_path;
                }
            }
        }

        const char* existing_path = std::getenv("PATH");
        if (existing_path && strlen(existing_path) > 0) {
            new_path = new_path + ";" + std::string(existing_path);
        }
        env_vars.push_back({"PATH", new_path});

        LOG(INFO, "SDServer") << "ROCm backend: added " << exe_dir.string() << " to PATH" << std::endl;
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

    LOG(INFO, "SDServer") << "Process started with PID: " << process_handle_.pid << std::endl;

    // Wait for server to be ready
    if (!wait_for_ready("/")) {
        unload();
        throw std::runtime_error("sd-server failed to start or become ready");
    }

    LOG(INFO, "SDServer") << "Server is ready at http://127.0.0.1:" << port_ << std::endl;
}

void SDServer::unload() {
    if (process_handle_.pid != 0) {
        LOG(INFO, "SDServer") << "Stopping server (PID: " << process_handle_.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
    image_defaults_ = ImageDefaults{};
}

json SDServer::build_extra_args(const json& request, bool include_flow_shift) const {
    // sd-server reads these from inside <sd_cpp_extra_args>{...}</sd_cpp_extra_args>
    // in the prompt (via SDGenerationParams::from_json_str). Top-level copies on
    // the HTTP body are ignored for everything except `size` / `n` / `prompt`, so
    // this is the only channel for step count, cfg scale, etc.
    //
    // sd-cpp master nests sample_steps / sample_method / scheduler / flow_shift
    // under a "sample_params" object, and cfg scale under "sample_params.guidance".
    // The old flat keys (`steps`, `cfg_scale`) are silently ignored, which is why
    // setting them at the top level of extra_args has no effect.
    //
    // Precedence for each value: request override -> model image_defaults
    // -> recipe_options.
    json extra_args;
    json sample_params = json::object();
    json guidance = json::object();

    auto resolve_int = [&](const std::string& key, int fallback) -> int {
        if (request.contains(key) && request[key].is_number_integer()) {
            return request[key].get<int>();
        }
        return fallback;
    };
    auto resolve_float = [&](const std::string& key, float fallback) -> float {
        if (request.contains(key) && request[key].is_number()) {
            return request[key].get<float>();
        }
        return fallback;
    };
    auto resolve_string = [&](const std::string& key, const std::string& fallback) -> std::string {
        if (request.contains(key) && request[key].is_string()) {
            return request[key].get<std::string>();
        }
        return fallback;
    };

    // steps -> sample_params.sample_steps
    int steps = image_defaults_.has_defaults
                  ? image_defaults_.steps
                  : static_cast<int>(recipe_options_.get_option("steps"));
    steps = resolve_int("steps", steps);
    if (steps > 0) {
        sample_params["sample_steps"] = steps;
    }

    // cfg_scale -> sample_params.guidance.txt_cfg
    float cfg_scale = image_defaults_.has_defaults
                        ? image_defaults_.cfg_scale
                        : static_cast<float>(recipe_options_.get_option("cfg_scale"));
    cfg_scale = resolve_float("cfg_scale", cfg_scale);
    if (cfg_scale > 0.0f) {
        guidance["txt_cfg"] = cfg_scale;
    }

    // sample_method -> sample_params.sample_method
    std::string sample_method;
    if (image_defaults_.has_defaults && !image_defaults_.sampling_method.empty()) {
        sample_method = image_defaults_.sampling_method;
    } else {
        sample_method = recipe_options_.get_option("sampling_method");
    }
    sample_method = resolve_string("sample_method", sample_method);
    if (!sample_method.empty()) {
        sample_params["sample_method"] = sample_method;
    }

    // flow_shift -> sample_params.flow_shift
    if (include_flow_shift) {
        float flow_shift = 0.0f;
        if (image_defaults_.has_defaults && image_defaults_.flow_shift > 0.0f) {
            flow_shift = image_defaults_.flow_shift;
        } else {
            float fs = recipe_options_.get_option("flow_shift");
            if (fs > 0.0f) flow_shift = fs;
        }
        flow_shift = resolve_float("flow_shift", flow_shift);
        if (flow_shift > 0.0f) {
            sample_params["flow_shift"] = flow_shift;
        }
    }

    if (!guidance.empty()) {
        sample_params["guidance"] = guidance;
    }
    if (!sample_params.empty()) {
        extra_args["sample_params"] = sample_params;
    }

    // seed stays top-level in from_json_str; preserve if the caller supplied one.
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        extra_args["seed"] = request["seed"].get<int>();
    }

    return extra_args;
}

std::string SDServer::resolve_size(const json& request) const {
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
    // Build request - sd-server uses OpenAI-compatible format.
    //
    // See PR #1173: https://github.com/leejet/stable-diffusion.cpp/pull/1173
    // for the <sd_cpp_extra_args> convention.
    json sd_request = request;

    // sd-server's /v1/images/generations reads size only from top-level
    // `size: "WxH"` -- separate `width`/`height` fields are silently dropped.
    // Collapse them here so both client styles work, and fill in image_defaults
    // when nothing is specified (important for Lemonade Model tool calls,
    // which pass only the prompt).
    std::string size = resolve_size(sd_request);
    sd_request.erase("width");
    sd_request.erase("height");
    if (!size.empty()) {
        sd_request["size"] = size;
    }

    json extra_args = build_extra_args(request);

    std::string prompt = sd_request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    sd_request["prompt"] = prompt;

    LOG(DEBUG, "SDServer") << "Forwarding request to sd-server: "
                  << sd_request.dump(2) << std::endl;

    // Image generation can take 20+ minutes for large models -- use global timeout
    return forward_request("/v1/images/generations", sd_request, utils::HttpClient::get_default_timeout());
}

json SDServer::image_edits(const json& request) {
    // Use sd-server's /v1/images/edits endpoint (EDIT mode).
    // Images are placed into ref_images, which works well with editing models
    // like Qwen-Edit and Flux Klein 4b/9b.
    // The endpoint expects multipart/form-data (like the OpenAI API).

    json extra_args = build_extra_args(request);

    std::string prompt = request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";

    std::vector<MultipartField> fields;
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});
    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    // Decode base64 image data back to binary for multipart upload
    if (request.contains("image_data")) {
        std::string image_binary = JsonUtils::base64_decode(
            request["image_data"].get<std::string>());
        fields.push_back({"image[]", image_binary, "image.png", "image/png"});
    }
    if (request.contains("mask_data")) {
        std::string mask_binary = JsonUtils::base64_decode(
            request["mask_data"].get<std::string>());
        fields.push_back({"mask", mask_binary, "mask.png", "image/png"});
    }

    LOG(DEBUG, "SDServer") << "Forwarding image edits to /v1/images/edits (multipart)"
                  << " prompt=" << prompt
                  << " n=" << request.value("n", 1)
                  << " size=" << size
                  << std::endl;

    return forward_multipart_request("/v1/images/edits", fields, utils::HttpClient::get_default_timeout());
}

json SDServer::image_variations(const json& request) {
    // The official OpenAI variations API does not take a prompt parameter,
    // but sd-server's /v1/images/edits implementation requires one. We therefore
    // send a synthetic "variation" prompt that embeds inference parameters so
    // the subprocess behaves consistently with our recipe_options defaults.
    json extra_args = build_extra_args(request, /*include_flow_shift=*/false);

    std::string prompt = "variation <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";

    std::vector<MultipartField> fields;
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});
    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    // Decode base64 image data back to binary for multipart upload
    if (request.contains("image_data")) {
        std::string image_binary = JsonUtils::base64_decode(
            request["image_data"].get<std::string>());
        fields.push_back({"image[]", image_binary, "image.png", "image/png"});
    }

    LOG(DEBUG, "SDServer") << "Forwarding image variations to /v1/images/edits (multipart)"
                  << " prompt=variation"
                  << " n=" << request.value("n", 1)
                  << " size=" << size
                  << std::endl;

    return forward_multipart_request("/v1/images/edits", fields, utils::HttpClient::get_default_timeout());
}

std::string SDServer::upscale_via_cli(
    const std::string& b64_image,
    const std::string& upscale_model_path,
    const std::string& cli_exe_path,
    const std::vector<std::pair<std::string, std::string>>& env_vars,
    bool debug) {

    if (!fs::exists(cli_exe_path)) {
        LOG(ERROR, "SDServer") << "sd-cli binary not found at: "
            << cli_exe_path << std::endl;
        return "";
    }

    std::string raw = JsonUtils::base64_decode(b64_image);

    auto unique_id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path temp_dir = fs::temp_directory_path() / "lemonade_upscale";
    fs::create_directories(temp_dir);
    fs::path input_path = temp_dir / ("input_" + unique_id + ".png");
    fs::path output_path = temp_dir / ("output_" + unique_id + ".png");

    struct TempFileGuard {
        fs::path path;
        ~TempFileGuard() { std::error_code ec; fs::remove(path, ec); }
    };
    TempFileGuard input_guard{input_path};
    TempFileGuard output_guard{output_path};

    {
        std::ofstream out(input_path, std::ios::binary);
        out.write(raw.data(), raw.size());
    }

    std::vector<std::string> cli_args = {
        "-M", "upscale",
        "--upscale-model", upscale_model_path,
        "-i", input_path.string(),
        "-o", output_path.string()
    };

    // inherit_output = true so subprocess stderr/stdout is visible in server
    // logs for debugging failed upscale operations
    auto proc = ProcessManager::start_process(
        cli_exe_path, cli_args, "", true, false, env_vars);

    int exit_code = ProcessManager::wait_for_exit(proc, 300);

    std::string result;
    if (exit_code == 0 && fs::exists(output_path)) {
        std::ifstream in(output_path, std::ios::binary);
        std::string upscaled_data(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        result = JsonUtils::base64_encode(upscaled_data);
        LOG(INFO, "SDServer") << "ESRGAN upscale complete ("
            << raw.size() << " -> " << upscaled_data.size() << " bytes)" << std::endl;
    } else {
        LOG(WARNING, "SDServer") << "ESRGAN upscale failed (exit code: "
            << exit_code << ", model: " << upscale_model_path
            << ", cli: " << cli_exe_path << ")" << std::endl;
    }

    return result;
}

} // namespace backends
} // namespace lemon
