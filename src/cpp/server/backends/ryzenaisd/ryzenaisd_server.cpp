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
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
#include <filesystem>
#include <set>
#include <sstream>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// ── Descriptor / spec / ops singletons ──────────────────────────────────────

namespace ryzenaisd {

// ryzenai-sd-server is locally built by the user — there is no GitHub release to
// auto-install. The binary path is resolved via the standard config.json
// external-binary mechanism: set ryzenaisd.npu to the absolute path of
// ryzenai-sd-server.exe in lemonade's config.json.
static const BackendSpec kSpec(
    descriptor.recipe,
    descriptor.binary,
    /*install_params_fn=*/nullptr   // no auto-install
);

// ryzenai-sd models are whole HuggingFace repos of pre-converted ONNX files.
// The server needs the snapshot root directory (contains text_encoder/, unet/,
// vae_decoder/ etc.), not a single file. Override resolve_checkpoint_path() to
// return the active snapshot directory rather than the model cache parent.
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

const BackendSpec* spec() { return &kSpec; }
const BackendOps*  ops()  { static const RyzenAISDOps kOps; return &kOps; }

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return std::make_unique<RyzenAISDServer>(
        ctx.log_level, ctx.model_manager, ctx.backend_manager);
}

}  // namespace ryzenaisd

// ── RyzenAISDServer ──────────────────────────────────────────────────────────

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

    // Capture the currently-loaded model's resolution (if any) before
    // image_defaults_/recipe_options_ get overwritten below -- needed to
    // decide whether a hot-swap is safe (see resolution_changed below).
    int old_width = image_defaults_.has_defaults
                       ? image_defaults_.width
                       : static_cast<int>(recipe_options_.get_option("width"));
    int old_height = image_defaults_.has_defaults
                        ? image_defaults_.height
                        : static_cast<int>(recipe_options_.get_option("height"));

    image_defaults_ = model_info.image_defaults;
    device_type_ = DEVICE_NPU;

    // The model path must be a directory containing the ONNX components.
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

    // If the subprocess is already running, hot-swap the model via
    // /v1/internal/load instead of killing and restarting the process.
    // This avoids the full startup cost (ONNX Runtime init, DLL loading, etc.)
    // when switching between ryzenai-sd models.
    ProcessHandle current_handle = get_process_handle_snapshot();
    bool process_alive = has_process_handle(current_handle) &&
                         utils::ProcessManager::is_running(current_handle);

    // The RyzenAI NPU EP holds a shared, process-wide instruction buffer sized
    // by whichever model loads first in the process; swapping to a different
    // resolution needs a different buffer size and crashes the process, even
    // though the swap itself is otherwise clean (observed empirically: same-
    // resolution hot-swaps -- e.g. SD-Turbo -> SD-1.5 at 512x512, or
    // SDXL-Base -> Segmind-Vega at 1024x1024 -- are reliable; cross-resolution
    // hot-swaps are not). Force a full process restart instead of a hot-swap
    // whenever the resolution changes, so the new process sizes its own
    // buffer fresh.
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
                                     << ") -- restarting process instead of hot-swapping" << std::endl;
        unload();
        process_alive = false;
    }

    if (process_alive) {
        LOG(INFO, "RyzenAISDServer") << "Process already running — hot-swapping model via /v1/internal/load" << std::endl;
        std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/v1/internal/load";
        json body = {{"model_path", model_path}};
        // TrustedLoopback is required here: HttpClient::post()'s default policy
        // (ExternalHttpsOnly) restricts curl to the "https" scheme only, which
        // rejects this plain-http loopback URL with CURLE_UNSUPPORTED_PROTOCOL.
        HttpResponse resp = HttpClient::post(url, body.dump(),
                                             {{"Content-Type", "application/json"}},
                                             /*timeout_seconds=*/300,
                                             utils::HttpSecurityPolicy::TrustedLoopback);
        if (resp.status_code != 200) {
            throw std::runtime_error("Failed to hot-swap model: " + resp.body);
        }
        // The hot-swap response only confirms the swap request was handled; it
        // doesn't guarantee the process is still alive and responsive (the NPU
        // EP can crash the process shortly after acking the swap). Apply the
        // same readiness check the cold-start path uses, just with a short
        // timeout since the process is already up.
        if (!wait_for_ready("/health", /*timeout_seconds=*/10)) {
            // The process is gone or wedged; clean up so the next load() call
            // takes the cold-start path instead of hot-swapping into a dead
            // process again.
            unload();
            throw std::runtime_error(
                "ryzenai-sd-server became unresponsive after hot-swapping to: " + model_path);
        }
        LOG(INFO, "RyzenAISDServer") << "Model hot-swapped to: " << model_path << std::endl;
        return;
    }

    // First load (or process died): start a fresh subprocess.
    std::string exe_path = BackendUtils::get_backend_binary_path(*ryzenaisd::spec(), "npu");

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

    // Apply custom user args, blocking flags that are managed by lemond.
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

    // Prepend the binary's directory to PATH so that the ONNX Runtime and
    // RyzenAI DLLs bundled alongside the executable are found by the loader.
    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef _WIN32
    fs::path exe_dir = fs::absolute(fs::path(exe_path)).parent_path();
    std::string new_path = path_to_utf8(exe_dir);
    const char* existing_path = std::getenv("PATH");
    if (existing_path && strlen(existing_path) > 0) {
        new_path += ";" + std::string(existing_path);
    }
    env_vars.push_back({"PATH", new_path});
    LOG(DEBUG, "RyzenAISDServer") << "Prepending exe dir to PATH: "
                                  << path_to_utf8(exe_dir) << std::endl;
#endif

    std::string working_dir;
#ifdef _WIN32
    working_dir = path_to_utf8(fs::absolute(fs::path(exe_path)).parent_path());
    std::string process_exe_path = path_to_utf8(fs::absolute(fs::path(exe_path)));
#else
    std::string process_exe_path = exe_path;
#endif

    ProcessHandle handle = utils::ProcessManager::start_process(
        process_exe_path,
        args,
        working_dir,
        is_debug(),   // inherit_output
        false,        // filter_health_logs
        env_vars
    );
    set_process_handle(handle, process_exe_path, args);

    if (!has_process_handle(handle)) {
        throw std::runtime_error("Failed to start ryzenai-sd-server process");
    }

    LOG(INFO, "RyzenAISDServer") << "Process started with PID: " << handle.pid << std::endl;

    // ryzenai-sd-server exposes GET /health → 200 when ready.
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("ryzenai-sd-server failed to start or become ready");
    }

    LOG(INFO, "RyzenAISDServer") << "Server ready at http://127.0.0.1:" << get_backend_port() << std::endl;
}

void RyzenAISDServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "RyzenAISDServer") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
    image_defaults_ = ImageDefaults{};
}

// ── Unsupported completion stubs ─────────────────────────────────────────────

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

// ── IImageServer ─────────────────────────────────────────────────────────────

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
    // Fall back to descriptor option defaults.
    int w = static_cast<int>(recipe_options_.get_option("width"));
    int h = static_cast<int>(recipe_options_.get_option("height"));
    if (w > 0 && h > 0) {
        return std::to_string(w) + "x" + std::to_string(h);
    }
    return "";
}

json RyzenAISDServer::build_extra_args(const json& request) const {
    // ryzenai-sd-server reads steps/cfg_scale/seed from a flat JSON object
    // embedded as <sd_cpp_extra_args>{...}</sd_cpp_extra_args> in the prompt
    // (see parse_extra_args() in ryzenai-sd-server's http_server.cpp). Unlike
    // sd-cpp, ryzenai-sd-server's schema is flat (no nested "sample_params").
    // Top-level body fields other than prompt/size/n are otherwise ignored, so
    // this is the only channel for step count, cfg scale, and seed on the
    // generations/edits endpoints.
    //
    // Precedence for each value: request override -> model image_defaults
    // -> recipe_options.
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

    // A negative/absent seed means "random"; leave it out so ryzenai-sd-server
    // falls back to its own std::random_device-seeded default.
    if (request.contains("seed") && request["seed"].is_number_integer() &&
        request["seed"].get<int>() >= 0) {
        extra_args["seed"] = request["seed"].get<int>();
    }

    return extra_args;
}

json RyzenAISDServer::image_generations(const json& request) {
    // ryzenai-sd-server accepts the OpenAI /v1/images/generations format.
    // Collapse separate width/height fields into the "size" string it expects.
    json sd_request = request;
    std::string size = resolve_size(request);
    sd_request.erase("width");
    sd_request.erase("height");
    if (!size.empty()) {
        sd_request["size"] = size;
    }

    // steps/cfg_scale/seed are only honored via the <sd_cpp_extra_args> tag
    // embedded in the prompt -- the server's hand-rolled JSON parser for this
    // endpoint only reads "prompt", "size", and "n" from the top-level JSON,
    // ignoring every other field. "n" (default 1) is read straight off
    // sd_request below since it's just a copy of the incoming request.
    json extra_args = build_extra_args(request);
    std::string prompt = sd_request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    sd_request["prompt"] = prompt;

    LOG(DEBUG, "RyzenAISDServer") << "Forwarding image generation request" << std::endl;
    // Image generation can be slow; use no timeout.
    return forward_request("/v1/images/generations", sd_request, 0);
}

json RyzenAISDServer::image_edits(const json& request) {
    // ryzenai-sd-server accepts multipart/form-data for /v1/images/edits.
    std::vector<MultipartField> fields;

    // Same as image_generations(): steps/cfg_scale/seed only take effect via
    // the <sd_cpp_extra_args> tag embedded in the prompt for this endpoint.
    json extra_args = build_extra_args(request);
    std::string prompt = request.value("prompt", "");
    prompt += " <sd_cpp_extra_args>" + extra_args.dump() + "</sd_cpp_extra_args>";
    fields.push_back({"prompt", prompt, "", ""});
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    // ryzenai-sd-server's /v1/images/edits reads "strength" as a plain
    // multipart field (img2img/inpainting denoising strength). Precedence:
    // request override -> recipe_options default.
    float strength = static_cast<float>(recipe_options_.get_option("strength"));
    if (request.contains("strength") && request["strength"].is_number()) {
        strength = request["strength"].get<float>();
    }
    fields.push_back({"strength", std::to_string(strength), "", ""});

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
    // Unlike generations/edits, ryzenai-sd-server's /v1/images/variations
    // endpoint natively accepts num_inference_steps/guidance_scale/seed/
    // strength as plain multipart fields (no <sd_cpp_extra_args> tag needed),
    // and its prompt is optional -- an empty/absent prompt falls back to the
    // server's own "image variation" default, giving true unprompted
    // OpenAI-style variation. Sending a literal placeholder prompt here would
    // bias every generation with that text.
    std::vector<MultipartField> fields;
    fields.push_back({"n", std::to_string(request.value("n", 1)), "", ""});

    std::string size = resolve_size(request);
    if (!size.empty()) {
        fields.push_back({"size", size, "", ""});
    }

    int steps = image_defaults_.has_defaults
                  ? image_defaults_.steps
                  : static_cast<int>(recipe_options_.get_option("steps"));
    if (request.contains("steps") && request["steps"].is_number_integer()) {
        steps = request["steps"].get<int>();
    }
    if (steps > 0) {
        fields.push_back({"num_inference_steps", std::to_string(steps), "", ""});
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
        fields.push_back({"guidance_scale", std::to_string(cfg_scale), "", ""});
    }

    if (request.contains("seed") && request["seed"].is_number_integer() &&
        request["seed"].get<int>() >= 0) {
        fields.push_back({"seed", std::to_string(request["seed"].get<int>()), "", ""});
    }

    // Precedence for strength: request override -> recipe_options default.
    float strength = static_cast<float>(recipe_options_.get_option("strength"));
    if (request.contains("strength") && request["strength"].is_number()) {
        strength = request["strength"].get<float>();
    }
    fields.push_back({"strength", std::to_string(strength), "", ""});

    if (request.contains("image_data")) {
        std::string img = JsonUtils::base64_decode(request["image_data"].get<std::string>());
        fields.push_back({"image[]", img, "image.png", "image/png"});
    }

    LOG(DEBUG, "RyzenAISDServer") << "Forwarding image variations request" << std::endl;
    return forward_multipart_request("/v1/images/variations", fields, 0);
}

}  // namespace backends
}  // namespace lemon
