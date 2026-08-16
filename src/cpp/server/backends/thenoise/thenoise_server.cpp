#include "lemon/backends/thenoise/thenoise_server.h"
#include "lemon/backends/thenoise/thenoise.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include <lemon/utils/aixlog.hpp>
#include <ctime>
#include <filesystem>
#include <random>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
int generate_random_seed() {
    return static_cast<int>(std::random_device{}() & 0x7fffffffU);
}

// Split a comma-separated LoRA spec string ("style:0.8,sub/detail:0.5") into an
// array of trimmed entries. Empty input yields an empty array.
json split_lora_specs(const std::string& specs) {
    json result = json::array();
    std::istringstream ss(specs);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t start = item.find_first_not_of(" \t");
        const size_t end = item.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        result.push_back(item.substr(start, end - start + 1));
    }
    return result;
}
}  // namespace

InstallParams TheNoiseServer::get_install_params(const std::string& backend, const std::string& version) {
    if (backend != "rocm") {
        throw std::runtime_error("TheNoise backend '" + backend + "' is not supported. Supported: rocm");
    }

    // TheNoise publishes one portable bundle per GPU target (gfx1150 / gfx1151)
    // under the same release tag. Pick the archive matching this host.
    std::string target_arch = SystemInfo::get_rocm_arch();

    InstallParams params;
    params.repo = "lemonade-sdk/thenoise";
    params.filename = version + "-" + target_arch + "-x64.tar.gz";
    return params;
}

TheNoiseServer::TheNoiseServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("thenoise-server", log_level, model_manager, backend_manager) {
    LOG(DEBUG, "TheNoise") << "Created with log_level=" << log_level << std::endl;
}

TheNoiseServer::~TheNoiseServer() {
    unload();
}

void TheNoiseServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool /* do_not_upgrade */) {
    LOG(INFO, "TheNoise") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "TheNoise") << "Per-model settings: " << options.to_log_string() << std::endl;

    image_defaults_ = model_info.image_defaults;

    std::string backend = options.get_option("thenoise_backend");
    if (backend.empty()) {
        backend = "rocm";
    }

    RuntimeConfig::validate_backend_choice("thenoise", backend);

    device_type_ = DEVICE_GPU;

    backend_manager_->install_backend(thenoise::spec()->recipe, backend);

    std::string dit_path = model_info.resolved_path("main");
    std::string vae_path = model_info.resolved_path("vae");
    std::string text_encoder_path = model_info.resolved_path("text_encoder");

    if (dit_path.empty()) {
        throw std::runtime_error("Model file not found for DiT checkpoint: " + model_info.checkpoint("main"));
    }
    if (!fs::exists(fs::path(dit_path))) {
        throw std::runtime_error("DiT model file does not exist: " + dit_path);
    }
    if (vae_path.empty() || !fs::exists(fs::path(vae_path))) {
        throw std::runtime_error("VAE model file not found: " + vae_path);
    }
    if (text_encoder_path.empty() || !fs::exists(fs::path(text_encoder_path))) {
        throw std::runtime_error("Text encoder model file not found: " + text_encoder_path);
    }

    LOG(DEBUG, "TheNoise") << "Using DiT: " << dit_path << std::endl;
    LOG(DEBUG, "TheNoise") << "Using VAE: " << vae_path << std::endl;
    LOG(DEBUG, "TheNoise") << "Using text encoder: " << text_encoder_path << std::endl;

    std::string exe_path = BackendUtils::get_backend_binary_path(*thenoise::spec(), backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "TheNoise") << "Starting server on port " << port_ << " (backend: " << backend << ")" << std::endl;

    std::vector<std::string> args = {
        "serve",
        "--dit", dit_path,
        "--vae", vae_path,
        "--text-encoder", text_encoder_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_)
    };

    // lora_dir / upscaler_dir are server-wide config (config.json
    // "thenoise" section) and are not overridable per model.
    std::string lora_dir;
    std::string upscaler_dir;
    if (auto* cfg = RuntimeConfig::global()) {
        lora_dir = cfg->backend_string("thenoise", "lora_dir");
        upscaler_dir = cfg->backend_string("thenoise", "upscaler_dir");
    }
    if (!lora_dir.empty()) {
        args.push_back("--lora-dir");
        args.push_back(lora_dir);
    }
    if (!upscaler_dir.empty()) {
        args.push_back("--upscaler-dir");
        args.push_back(upscaler_dir);
    }

    // The portable thenoise launcher sets up LD_LIBRARY_PATH / CC / ROCm env itself.
    std::vector<std::pair<std::string, std::string>> env_vars;

    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",
        is_debug(),  // inherit_output
        false,  // filter_health_logs
        env_vars
    );
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start thenoise process");
    }

    LOG(INFO, "TheNoise") << "Process started with PID: " << started_handle.pid << std::endl;

    // thenoise compiles the DiT with torch.compile on first load, which can take
    // several minutes; give it a generous startup window.
    if (!wait_for_ready("/health", 1800)) {
        unload();
        throw std::runtime_error("thenoise failed to start or become ready");
    }

    LOG(INFO, "TheNoise") << "Server is ready at http://127.0.0.1:" << get_backend_port() << std::endl;
}

void TheNoiseServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "TheNoise") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
    image_defaults_ = ImageDefaults{};
}

// ICompletionServer implementation - not supported for image generation
json TheNoiseServer::chat_completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Chat completion", "thenoise (image generation model)")
    );
}

json TheNoiseServer::completion(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Text completion", "thenoise (image generation model)")
    );
}

json TheNoiseServer::responses(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses", "thenoise (image generation model)")
    );
}

std::string TheNoiseServer::resolve_size(const json& request) const {
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

json TheNoiseServer::build_request(const json& request) const {
    // thenoise /text2image accepts the shared model-agnostic params directly.
    // Precedence for each value: request override -> model image_defaults
    // -> recipe_options.
    json body;

    body["prompt"] = request.value("prompt", "");

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
    auto resolve_bool = [&](const std::string& key, bool fallback) -> bool {
        if (request.contains(key) && request[key].is_boolean()) {
            return request[key].get<bool>();
        }
        return fallback;
    };

    // size -> width / height
    std::string size = resolve_size(request);
    if (!size.empty()) {
        size_t x = size.find('x');
        if (x != std::string::npos) {
            try { body["width"] = std::stoi(size.substr(0, x)); } catch (...) {}
            try { body["height"] = std::stoi(size.substr(x + 1)); } catch (...) {}
        }
    }

    // negative_prompt
    std::string negative_prompt = resolve_string("negative_prompt",
        recipe_options_.get_option("negative_prompt"));
    if (!negative_prompt.empty()) {
        body["negative_prompt"] = negative_prompt;
    }

    // steps
    int steps = image_defaults_.has_defaults
                  ? image_defaults_.steps
                  : static_cast<int>(recipe_options_.get_option("steps"));
    steps = resolve_int("steps", steps);
    if (steps > 0) {
        body["steps"] = steps;
    }

    // cfg_scale -> guidance_scale
    float cfg_scale = image_defaults_.has_defaults
                              ? image_defaults_.cfg_scale
                              : static_cast<float>(recipe_options_.get_option("cfg_scale"));
    cfg_scale = resolve_float("cfg_scale", cfg_scale);
    if (cfg_scale > 0.0f) {
        body["guidance_scale"] = cfg_scale;
    }

    // seed stays as-is; negative means "random" for thenoise. We still emit a
    // concrete seed so a user-supplied negative never accidentally collides.
    if (request.contains("seed") && request["seed"].is_number_integer()) {
        int seed = request["seed"].get<int>();
        body["seed"] = seed >= 0 ? seed : generate_random_seed();
    }

    // sampler
    std::string sampler = resolve_string("sampler", recipe_options_.get_option("sampler"));
    if (!sampler.empty()) {
        body["sampler"] = sampler;
    }

    // qwen_vae_enhance
    bool qwen_vae_enhance = resolve_bool("qwen_vae_enhance",
        recipe_options_.get_option("qwen_vae_enhance"));
    if (qwen_vae_enhance) {
        body["qwen_vae_enhance"] = true;
    }

    // film_grain
    float film_grain = static_cast<float>(recipe_options_.get_option("film_grain"));
    film_grain = resolve_float("film_grain", film_grain);
    if (film_grain > 0.0f) {
        body["film_grain"] = film_grain;
    }

    // sharpening
    float sharpening = static_cast<float>(recipe_options_.get_option("sharpening"));
    sharpening = resolve_float("sharpening", sharpening);
    if (sharpening > 0.0f) {
        body["sharpening"] = sharpening;
    }

    // lora_specs: config/recipe stores a comma-separated string; forward as an
    // array. A client request may pass an array or a string directly.
    if (request.contains("lora_specs") && request["lora_specs"].is_array()) {
        body["lora_specs"] = request["lora_specs"];
    } else if (request.contains("lora_specs") && request["lora_specs"].is_string()) {
        json specs = split_lora_specs(request["lora_specs"].get<std::string>());
        if (!specs.empty()) body["lora_specs"] = specs;
    } else {
        const json opt = recipe_options_.get_option("lora_specs");
        std::string specs = opt.is_string() ? opt.get<std::string>() : "";
        if (!specs.empty()) body["lora_specs"] = split_lora_specs(specs);
    }

    // Pass through any request parameters this adapter does not map so they
    // reach the backend untouched. Skip every key the adapter handles (derived
    // from the descriptor options) plus the ones it consumes/transforms that
    // are not descriptor options, so raw forms are never re-forwarded.
    std::set<std::string> handled;
    for (const auto& opt : thenoise::descriptor.options) {
        handled.insert(opt.name);
    }
    for (const char* key : {
        "prompt", "seed", "size", "model", "n"
    }) {
        handled.insert(key);
    }
    for (auto& [key, value] : request.items()) {
        if (handled.count(key)) continue;
        body[key] = value;
    }

    return body;
}

json TheNoiseServer::image_generations(const json& request) {
    int n = request.value("n", 1);
    if (n < 1) n = 1;

    json data = json::array();
    for (int i = 0; i < n; ++i) {
        json body = build_request(request);
        body["out"] = "json";

        LOG(DEBUG, "TheNoise") << "Forwarding request to thenoise: " << body.dump(2) << std::endl;

        json resp = forward_request("/text2image", body);

        if (!resp.contains("b64_json")) {
            LOG(ERROR, "TheNoise") << "thenoise image generation failed: " << resp.dump() << std::endl;
            return ErrorResponse::from_exception(
                BackendException("thenoise", "image generation returned no b64_json: " + resp.dump())
            );
        }

        data.push_back(resp);
    }

    return {{"created", static_cast<long long>(std::time(nullptr))}, {"data", data}};
}

json TheNoiseServer::image_edits(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Image editing", "thenoise (text-to-image only)")
    );
}

json TheNoiseServer::image_variations(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Image variations", "thenoise (text-to-image only)")
    );
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace thenoise {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<TheNoiseServer>(ctx);
}


const BackendSpec* spec() { return make_spec<TheNoiseServer>(descriptor, /*split=*/true); }
const BackendOps* ops() { return default_backend_ops(); }
}  // namespace thenoise
}  // namespace backends
}  // namespace lemon
