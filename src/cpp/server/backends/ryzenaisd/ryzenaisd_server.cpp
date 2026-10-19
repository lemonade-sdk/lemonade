#include "lemon/backends/ryzenaisd/ryzenaisd_server.h"
#include "lemon/backends/ryzenaisd/ryzenaisd.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/error_types.h"
#include <filesystem>
#include <set>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace ryzenaisd {

// The server needs the snapshot root directory (text_encoder/, unet/,
// vae_decoder/ ...), not a single checkpoint file.
class RyzenAISDOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        (void)info;
        if (ctx.type == "npu_cache") return "";
        fs::path cache = path_from_utf8(ctx.model_cache_path);
        fs::path snapshot = hf_cache::active_snapshot_path(cache);
        if (!snapshot.empty() && fs::exists(snapshot))
            return path_to_utf8(snapshot);
        return "";
    }
};

const BackendSpec* spec() { return make_spec<RyzenAISDServer>(descriptor); }
const BackendOps*  ops()  { static const RyzenAISDOps kOps; return &kOps; }

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return std::make_unique<RyzenAISDServer>(
        ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

}  // namespace ryzenaisd

InstallParams RyzenAISDServer::get_install_params(const std::string& /*backend*/,
                                                  const std::string& /*version*/) {
    return {"lemonade-sdk/ryzenai-sd-server", "ryzenai-sd-server.zip"};
}

RyzenAISDServer::RyzenAISDServer(const std::string& log_level,
                                 ModelManager* model_manager,
                                 BackendManager* backend_manager)
    : WrappedServer("ryzenai-sd-server", log_level, model_manager, backend_manager) {
    LOG(DEBUG, "RyzenAISDServer") << "Created with log_level=" << log_level << std::endl;
}

RyzenAISDServer::~RyzenAISDServer() {
    unload();
}

void RyzenAISDServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool /* do_not_upgrade */) {
    LOG(INFO, "RyzenAISDServer") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "RyzenAISDServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    // Read the outgoing model's resolution before image_defaults_ is overwritten.
    int old_width = image_defaults_.has_defaults
                       ? image_defaults_.width
                       : static_cast<int>(recipe_options_.get_option("width"));
    int old_height = image_defaults_.has_defaults
                        ? image_defaults_.height
                        : static_cast<int>(recipe_options_.get_option("height"));

    image_defaults_ = model_info.image_defaults;
    device_type_ = DEVICE_NPU;

    std::string model_path = model_info.resolved_path("main");
    if (model_path.empty()) {
        throw std::runtime_error("Model directory not found for: " + model_info.checkpoint());
    }
    if (!fs::exists(model_path)) {
        throw std::runtime_error("Model path does not exist: " + model_path);
    }
    if (!fs::is_directory(model_path)) {
        throw std::runtime_error("Model path must be a directory, got: " + model_path);
    }

    LOG(DEBUG, "RyzenAISDServer") << "Using model directory: " << model_path << std::endl;

    // Reusing a live subprocess skips ONNX Runtime init and DLL loading, which
    // dominate startup. The RyzenAI NPU EP sizes a process-wide instruction
    // buffer from the first model loaded, so a resolution change needs a fresh
    // process; same-resolution swaps are reliable.
    bool process_alive = is_backend_alive();

    int new_width = model_info.image_defaults.has_defaults
                       ? model_info.image_defaults.width
                       : static_cast<int>(options.get_option("width"));
    int new_height = model_info.image_defaults.has_defaults
                        ? model_info.image_defaults.height
                        : static_cast<int>(options.get_option("height"));
    bool resolution_changed = old_width > 0 && old_height > 0 &&
                              (old_width != new_width || old_height != new_height);

    if (process_alive && resolution_changed) {
        LOG(INFO, "RyzenAISDServer") << "Resolution changed (" << old_width << "x" << old_height
                                     << " -> " << new_width << "x" << new_height
                                     << ") - restarting instead of hot-swapping" << std::endl;
        unload();
        process_alive = false;
    }

    if (process_alive) {
        LOG(INFO, "RyzenAISDServer") << "Process already running — hot-swapping model via /v1/internal/load" << std::endl;
        std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/v1/internal/load";
        json body = {{"model_path", model_path}};
        // The default ExternalHttpsOnly policy would reject this plain-http URL.
        HttpResponse resp = HttpClient::post(url, body.dump(),
                                             {{"Content-Type", "application/json"}},
                                             /*timeout_seconds=*/300,
                                             utils::HttpSecurityPolicy::TrustedLoopback);
        if (resp.status_code != 200) {
            throw std::runtime_error("Failed to hot-swap model: " + resp.body);
        }
        // A 200 only acks the swap request; the NPU EP can still crash the
        // process while re-initializing.
        if (!wait_for_ready("/health", /*timeout_seconds=*/10)) {
            // Clear the dead process so the next load() cold-starts.
            unload();
            throw std::runtime_error(
                "ryzenai-sd-server became unresponsive after hot-swapping to: " + model_path);
        }
        LOG(INFO, "RyzenAISDServer") << "Model hot-swapped to: " << model_path << std::endl;
        return;
    }

    backend_manager_->install_backend("ryzenai-sd", "npu");

    std::string exe_path = BackendUtils::get_backend_binary_path(*ryzenaisd::spec(), "npu");
    if (exe_path.empty()) {
        throw std::runtime_error("ryzenai-sd-server executable not found even after installation attempt");
    }

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "RyzenAISDServer") << "Starting server on port " << port_ << std::endl;

    std::vector<std::string> args = {
        "--server",
        "--model-path", model_path,
        "--listen-port", std::to_string(port_),
    };

    if (is_debug()) {
        args.push_back("--verbose");
    }

    std::string ryzenaisd_args = options.get_option("ryzenaisd_args");
    if (!ryzenaisd_args.empty()) {
        const std::set<std::string> reserved = {
            "--server", "--model-path", "-m", "--listen-port", "--port",
            "--verbose", "-V",
        };
        std::string err = validate_custom_args(ryzenaisd_args, reserved);
        if (!err.empty()) {
            throw std::invalid_argument("Invalid ryzenaisd_args:\n" + err);
        }
        LOG(DEBUG, "RyzenAISDServer") << "Adding custom args: " << ryzenaisd_args << std::endl;
        auto custom = parse_custom_args(ryzenaisd_args);
        args.insert(args.end(), custom.begin(), custom.end());
    }

    ServerCommand command;
    command.program = path_to_utf8(fs::absolute(fs::path(exe_path)));
    command.args = std::move(args);
    command.port = port_;

    // The ONNX Runtime and RyzenAI DLLs ship next to the executable, not on the
    // system path, so its directory has to lead for the loader to find them.
    const std::string exe_dir = path_to_utf8(fs::absolute(fs::path(exe_path)).parent_path());
#ifdef _WIN32
    std::string new_path = exe_dir;
    if (const char* existing_path = std::getenv("PATH")) {
        if (existing_path[0] != '\0') {
            new_path += ";" + std::string(existing_path);
        }
    }
    command.env.push_back({"PATH", new_path});
#endif

    start_server(std::make_unique<NativeProcess>(ProcessOutput{is_debug(), false}, exe_dir),
                 command);

    LOG(INFO, "RyzenAISDServer") << "Server ready at http://127.0.0.1:" << get_backend_port() << std::endl;
}

void RyzenAISDServer::unload() {
    stop_server();
    image_defaults_ = ImageDefaults{};
}

json RyzenAISDServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "ryzenai-sd (image generation model)"));
}

json RyzenAISDServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "ryzenai-sd (image generation model)"));
}

json RyzenAISDServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "ryzenai-sd (image generation model)"));
}

std::string RyzenAISDServer::resolve_size(const json& request) const {
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
    int w = static_cast<int>(recipe_options_.get_option("width"));
    int h = static_cast<int>(recipe_options_.get_option("height"));
    if (w > 0 && h > 0) {
        return std::to_string(w) + "x" + std::to_string(h);
    }
    return "";
}

json RyzenAISDServer::build_extra_args(const json& request) const {
    json extra_args;

    int steps = image_defaults_.has_defaults
                  ? image_defaults_.steps
                  : static_cast<int>(recipe_options_.get_option("steps"));
    if (request.contains("steps") && request["steps"].is_number_integer()) {
        steps = request["steps"].get<int>();
    }
    if (steps > 0) {
        extra_args["steps"] = steps;
    }

    float cfg_scale = image_defaults_.has_defaults
                         ? image_defaults_.cfg_scale
                         : static_cast<float>(recipe_options_.get_option("cfg_scale"));
    if (request.contains("cfg_scale") && request["cfg_scale"].is_number()) {
        cfg_scale = request["cfg_scale"].get<float>();
    } else if (request.contains("guidance_scale") && request["guidance_scale"].is_number()) {
        cfg_scale = request["guidance_scale"].get<float>();
    }
    if (cfg_scale > 0.0f) {
        extra_args["cfg_scale"] = cfg_scale;
    }

    // A negative/absent seed means "random": omit it so the subprocess picks one.
    if (request.contains("seed") && request["seed"].is_number_integer() &&
        request["seed"].get<int>() >= 0) {
        extra_args["seed"] = request["seed"].get<int>();
    }

    return extra_args;
}

json RyzenAISDServer::image_generations(const json& request) {
    // Collapse width/height into the "size" string the subprocess expects.
    json sd_request = request;
    std::string size = resolve_size(request);
    sd_request.erase("width");
    sd_request.erase("height");
    if (!size.empty()) {
        sd_request["size"] = size;
    }

    json extra_args = build_extra_args(request);
    std::string prompt = sd_request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    sd_request["prompt"] = prompt;

    LOG(DEBUG, "RyzenAISDServer") << "Forwarding image generation request" << std::endl;
    return forward_request("/v1/images/generations", sd_request, 0);
}

json RyzenAISDServer::image_edits(const json& request) {
    std::vector<MultipartField> fields;

    json extra_args = build_extra_args(request);
    float strength = static_cast<float>(recipe_options_.get_option("strength"));
    if (request.contains("strength") && request["strength"].is_number()) {
        strength = request["strength"].get<float>();
    }
    extra_args["strength"] = strength;

    std::string prompt = request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    if (request.contains("image_data")) {
        std::string img = JsonUtils::base64_decode(request["image_data"].get<std::string>());
        fields.push_back({"image[]", img, "image.png", "image/png"});
    }
    if (request.contains("mask_data")) {
        std::string mask = JsonUtils::base64_decode(request["mask_data"].get<std::string>());
        fields.push_back({"mask", mask, "mask.png", "image/png"});
    }

    LOG(DEBUG, "RyzenAISDServer") << "Forwarding image edits request" << std::endl;
    return forward_multipart_request("/v1/images/edits", fields, 0);
}

json RyzenAISDServer::image_variations(const json& request) {
    // The prompt stays optional here: an empty one lets the subprocess apply
    // its own variation default. A placeholder would bias every generation.
    json extra_args = build_extra_args(request);
    float strength = static_cast<float>(recipe_options_.get_option("strength"));
    if (request.contains("strength") && request["strength"].is_number()) {
        strength = request["strength"].get<float>();
    }
    extra_args["strength"] = strength;

    std::string prompt = request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";

    std::vector<MultipartField> fields;
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    if (request.contains("image_data")) {
        std::string img = JsonUtils::base64_decode(request["image_data"].get<std::string>());
        fields.push_back({"image[]", img, "image.png", "image/png"});
    }

    LOG(DEBUG, "RyzenAISDServer") << "Forwarding image variations request" << std::endl;
    return forward_multipart_request("/v1/images/variations", fields, 0);
}

}  // namespace backends
}  // namespace lemon
