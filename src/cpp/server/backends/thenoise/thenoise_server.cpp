#include "lemon/backends/thenoise/thenoise_server.h"
#include "lemon/backends/thenoise/thenoise.h"
#include "lemon/backends/thenoise/thenoise_bundle.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include "lemon/model_types.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <utility>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>

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

std::string resolve_thenoise_backend(const std::string& requested) {
    if (!requested.empty() && requested != "auto") return requested;
    const auto supported = SystemInfo::get_supported_backends("thenoise");
    if (supported.backends.empty()) {
        throw std::runtime_error("No supported TheNoise GPU backend: " + supported.not_supported_error);
    }
    return supported.backends.front();
}

#ifdef _WIN32
void launch_bundled_python(std::string& exe_path,
                           std::vector<std::string>& args,
                           std::vector<std::pair<std::string, std::string>>& env_vars) {
    const thenoise_bundle::Launch launch = thenoise_bundle::make_launch(
        exe_path, get_environment_variable_utf8("PATH"));
    exe_path = launch.executable;
    args.insert(args.begin(), launch.args.begin(), launch.args.end());
    env_vars = launch.env_vars;
}
#endif
}  // namespace

InstallParams TheNoiseServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/thenoise";
    if (backend == "cuda") {
#if defined(__linux__) && defined(__x86_64__)
        params.filename = version + "-cuda-x64.tar.gz";
        return params;
#else
        throw std::runtime_error("TheNoise CUDA is supported on Linux x86_64 only");
#endif
    }
    if (backend != "rocm") {
        throw std::runtime_error("TheNoise backend '" + backend + "' is not supported. Supported: rocm, cuda (Linux)");
    }

    std::string target_arch = SystemInfo::rocm_asset_family(SystemInfo::get_rocm_arch());
#ifdef _WIN32
    params.filename = version + "-" + target_arch + "-win-x64.zip";
#else
    params.filename = version + "-" + target_arch + "-x64.tar.gz";
#endif
    return params;
}

TheNoiseServer::TheNoiseServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("thenoise-server", log_level, model_manager, backend_manager) {
    LOG(DEBUG, "TheNoise") << "Created with log_level=" << log_level << std::endl;
}

void TheNoiseServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool /* do_not_upgrade */) {
    LOG(INFO, "TheNoise") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "TheNoise") << "Per-model settings: " << options.to_log_string() << std::endl;

    const std::string backend = resolve_thenoise_backend(options.get_option("thenoise_backend"));

    RuntimeConfig::validate_backend_choice("thenoise", backend);
    const std::string selected_device = options.get_option("thenoise_device");

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
    // Video adapters may carry the VAE inside the main checkpoint (LTX 2.3).
    // Preserve the image requirement and never ignore a declared missing file.
    const bool vae_required = get_model_type_from_labels(model_info.labels) != ModelType::VIDEO ||
                              !model_info.checkpoint("vae").empty();
    if ((vae_required || !vae_path.empty()) &&
        (vae_path.empty() || !fs::exists(path_from_utf8(vae_path)))) {
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
        "--text-encoder", text_encoder_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_)
    };

    if (!vae_path.empty()) args.insert(args.end(), {"--vae", vae_path});

    // Optional components are named by role in the catalog. TheNoise decides
    // which ones its adapter needs (e.g. LTX 2.5 packs the text projections
    // into its text encoder). Do not infer components from checkpoint names.
    const std::pair<const char*, const char*> video_components[] = {
        {"dit_low", "--dit-low"},
        {"audio_vae", "--audio-vae"},
        {"spatial_upscaler", "--spatial-upscaler"},
        {"text_projection", "--text-projection"},
    };
    for (const auto& [role, flag] : video_components) {
        if (model_info.checkpoint(role).empty()) continue;
        const auto path = model_info.resolved_path(role);
        if (path.empty() || !fs::is_regular_file(path_from_utf8(path))) {
            throw std::runtime_error(std::string("TheNoise checkpoint '") + role +
                                     "' not found: " + model_info.checkpoint(role));
        }
        args.insert(args.end(), {flag, path});
    }

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

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef _WIN32
    launch_bundled_python(exe_path, args, env_vars);
    LOG(DEBUG, "TheNoise") << "Launching bundled interpreter: " << exe_path << std::endl;
#endif
    BackendUtils::apply_accelerator_device_env_vars(env_vars, "TheNoise", backend, selected_device);

    ServerCommand command;
    command.program = exe_path;
    command.args = std::move(args);
    command.env = std::move(env_vars);
    command.port = port_;
    // thenoise compiles the DiT with torch.compile on first load, which can take
    // several minutes; give it a generous startup window.
    start_server(std::make_unique<NativeProcess>(ProcessOutput{is_debug(), false}), command, 1800);
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
    const auto w = recipe_options_.stored_int_option("width");
    const auto h = recipe_options_.stored_int_option("height");
    if (!w || !h || *w <= 0 || *h <= 0) return "";
    return std::to_string(*w) + "x" + std::to_string(*h);
}

json TheNoiseServer::build_request(const json& request) const {
    // thenoise /text2image accepts the shared model-agnostic params directly.
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
        recipe_options_.string_option("negative_prompt").value_or(""));
    if (!negative_prompt.empty()) {
        body["negative_prompt"] = negative_prompt;
    }

    // steps
    int steps = resolve_int("steps", recipe_options_.int_option("steps").value_or(0));
    if (steps > 0) {
        body["steps"] = steps;
    }

    // cfg_scale -> guidance_scale
    float cfg_scale = resolve_float("cfg_scale",
                                    static_cast<float>(recipe_options_.number_option("cfg_scale").value_or(0.0)));
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
    std::string sampler = resolve_string("sampler", recipe_options_.string_option("sampler").value_or(""));
    if (!sampler.empty()) {
        body["sampler"] = sampler;
    }

    // qwen_vae_enhance
    bool qwen_vae_enhance = resolve_bool("qwen_vae_enhance",
        recipe_options_.bool_option("qwen_vae_enhance").value_or(false));
    if (qwen_vae_enhance) {
        body["qwen_vae_enhance"] = true;
    }

    // film_grain
    float film_grain = static_cast<float>(recipe_options_.number_option("film_grain").value_or(0.0));
    film_grain = resolve_float("film_grain", film_grain);
    if (film_grain > 0.0f) {
        body["film_grain"] = film_grain;
    }

    // sharpening
    float sharpening = static_cast<float>(recipe_options_.number_option("sharpening").value_or(0.0));
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
        std::string specs = recipe_options_.string_option("lora_specs").value_or("");
        if (!specs.empty()) body["lora_specs"] = split_lora_specs(specs);
    }

    // Pass through any request parameters this adapter does not map so they
    // reach the backend untouched. Skip every key the adapter handles (derived
    // from the descriptor options) plus the ones it consumes/transforms that
    // are not descriptor options, so raw forms are never re-forwarded.
    std::set<std::string> handled;
    for (const auto& opt : thenoise::descriptor.options) {
        // These new video recipe options are not consumed by this image adapter.
        if (opt.name == std::string("video_frames") || opt.name == std::string("fps") ||
            opt.name == std::string("flow_shift")) continue;
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

json TheNoiseServer::image_edits(const json& request) {
    int n = request.value("n", 1);
    if (n < 1) n = 1;

    json data = json::array();
    for (int i = 0; i < n; ++i) {
        json body = build_request(request);
        body["out"] = "json";

        // build_request passes unknown fields through; strip internal upload data.
        body.erase("image_files");
        body.erase("image_data");
        body.erase("image_filename");
        body.erase("image_content_type");
        if (request.contains("image_files") && request["image_files"].is_array()
            && !request["image_files"].empty()) {
            json references = json::array();
            for (const auto& image : request["image_files"]) {
                references.push_back(image.at("data"));
            }
            // TheNoise accepts a single base64 string or an array of strings.
            body["image"] = references.size() == 1 ? references[0] : references;
        } else if (request.contains("image_data") && !request["image_data"].is_null()) {
            body["image"] = request["image_data"];
        }

        LOG(DEBUG, "TheNoise") << "Forwarding image edit to thenoise: " << body.dump(2) << std::endl;

        json resp = forward_request("/edit", body);

        if (!resp.contains("b64_json")) {
            LOG(ERROR, "TheNoise") << "thenoise image edit failed: " << resp.dump() << std::endl;
            return ErrorResponse::from_exception(
                BackendException("thenoise", "image edit returned no b64_json: " + resp.dump())
            );
        }

        data.push_back(resp);
    }

    return {{"created", static_cast<long long>(std::time(nullptr))}, {"data", data}};
}

json TheNoiseServer::image_variations(const json& /* request */) {
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Image variations", "thenoise")
    );
}

json TheNoiseServer::video_generations(const json& request, std::atomic<bool>* cancel,
                                       const ProgressFn& progress) {
    constexpr long control_timeout = 5;
    constexpr int max_control_failures = 3;
    constexpr auto cancel_grace = std::chrono::minutes(5);
    const auto canceled = [&] { return cancel && cancel->load(); };
    const auto error = [&](const std::string& message, int status = 0) {
        return BackendException("thenoise", message, status).to_json();
    };
    const auto canceled_result = [&](bool settled) {
        auto result = error("video generation canceled", 499);
        // The job service can keep a model it loaded for this request warm when
        // the backend has acknowledged that there is no inference left running.
        if (settled) result["error"]["details"]["backend_job_settled"] = true;
        return result;
    };
    if (canceled()) return canceled_result(true);

    // VideoSpec is strict: do not forward the image request, Lemonade's model
    // alias, ctx_size, clip_file or other job bookkeeping. Only explicit recipe
    // values override native video defaults (the descriptor's image defaults
    // are not appropriate for video).
    const auto options = get_recipe_options();
    json body = {{"prompt", request.value("prompt", std::string())}};
    const auto supplied = [&](const char* key) {
        return request.contains(key) && !request[key].is_null();
    };
    const auto option = [&](const char* key, const char* target, bool zero_is_unset = false) {
        if (supplied(key)) {
            body[target] = request[key];
        } else if (options.has_option(key)) {
            const auto value = options.get_option(key);
            if (!value.is_null() && !(zero_is_unset && value.is_number() && value == 0)) {
                body[target] = value;
            }
        }
    };
    for (const char* key : {"width", "height", "steps", "negative_prompt"}) {
        option(key, key);
    }
    option("fps", "fps", true);
    option("flow_shift", "flow_shift", true);
    option("cfg_scale", "guidance_scale");
    // Explicit frame count > explicit duration > stored frame count > native default.
    if (supplied("video_frames")) {
        body["frames"] = request["video_frames"];
    } else if (supplied("duration")) {
        body["duration"] = request["duration"];
    } else {
        option("video_frames", "frames", true);
    }
    if (supplied("sampling_method")) {
        body["sampler"] = request["sampling_method"];
    } else {
        option("sampler", "sampler");
        if (body.contains("sampler") && body["sampler"] == "") body.erase("sampler");
    }
    if (supplied("seed")) {
        // Lemonade accepts any negative seed to request random noise.
        body["seed"] = request["seed"] < 0 ? json(-1) : request["seed"];
    }
    if (supplied("lora_specs")) {
        body["lora_specs"] = request["lora_specs"].is_string()
            ? split_lora_specs(request["lora_specs"].get<std::string>()) : request["lora_specs"];
    } else if (options.has_option("lora_specs")) {
        const auto specs = options.string_option("lora_specs");
        if (specs && !specs->empty()) body["lora_specs"] = split_lora_specs(*specs);
    }
    const auto format = supplied("output_format") && request["output_format"].is_string()
        ? request["output_format"].get<std::string>() : std::string("webm");
    if ((supplied("output_format") && !request["output_format"].is_string()) ||
        (format != "webm" && format != "mp4")) {
        return ErrorResponse::from_exception(
            InvalidRequestException("TheNoise video supports output_format webm or mp4"));
    }
    body["output_format"] = format;

    if (!is_backend_alive()) return error("backend is not running");
    BackendRequestScope scope(*this, BackendRequestKind::NonStreaming);
    const auto post = [&](const std::string& path, const json& payload) {
        auto response = HttpClient::post(get_base_url() + path, payload.dump(),
                                        {{"Content-Type", "application/json"}}, control_timeout,
                                        HttpSecurityPolicy::TrustedLoopback);
        note_backend_activity();
        return response;
    };
    const auto get = [&](const std::string& path, long timeout,
                         const RequestCancelToken& token = {}) {
        auto response = HttpClient::get(get_base_url() + path, {}, timeout,
                                       HttpSecurityPolicy::TrustedLoopback, 0, token, false);
        note_backend_activity();
        return response;
    };
    // Losing the handle or repeatedly losing contact is exceptional: do not
    // leave an orphaned job consuming the GPU, and do not replay the submit.
    const auto lost_job = [&](const std::string& message) {
        LOG(WARNING, "TheNoise") << "Video job state unknown: " << message << std::endl;
        stop_server();
        return canceled() ? canceled_result(false) : error(message);
    };

    std::string job_path;
    try {
        // Keep the short submit alive even if cancel is requested in flight;
        // we need its ID to cancel the accepted job rather than kill the model.
        if (canceled()) return canceled_result(true);
        const auto response = post("/jobs/video", body);
        if (response.status_code != 202) {
            const auto message = "video submission failed (HTTP " + std::to_string(response.status_code) + "): " + response.body;
            if (response.status_code >= 400 && response.status_code < 500) {
                return canceled() ? canceled_result(true) : error(message, response.status_code);
            }
            return lost_job(message);
        }
        const auto id = json::parse(response.body).at("id").get<std::string>();
        if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
            return lost_job("invalid backend video job ID");
        }
        job_path = "/jobs/" + id;
    } catch (const std::exception& e) {
        return lost_job(std::string("video submission failed: ") + e.what());
    }

    bool cancel_sent = false;
    int cancel_failures = 0;
    int poll_failures = 0;
    auto cancel_deadline = std::chrono::steady_clock::time_point::max();
    json status;
    for (;;) {
        if (canceled() && cancel_deadline == std::chrono::steady_clock::time_point::max()) {
            // A GPU stage can legitimately take time to reach its next check.
            cancel_deadline = std::chrono::steady_clock::now() + cancel_grace;
        }
        if (canceled() && !cancel_sent) {
            try {
                const auto response = post(job_path + "/cancel", json::object());
                // 409 also occurs if a previous cancel reached the backend or
                // completion won the race. Poll for the actual terminal state.
                cancel_sent = response.status_code == 200 || response.status_code == 409;
            } catch (const std::exception&) {
                // The request may have reached TheNoise; polling is still safe.
            }
            if (!cancel_sent) ++cancel_failures;
        }
        try {
            const auto response = get(job_path, control_timeout);
            if (response.status_code != 200) {
                throw std::runtime_error("video status HTTP " + std::to_string(response.status_code));
            }
            status = json::parse(response.body);
            const auto state = status.at("status").get<std::string>();
            if (state == "completed" || state == "failed" || state == "canceled") break;
            if (state != "queued" && state != "running") {
                throw std::runtime_error("unexpected video job state: " + state);
            }
            poll_failures = 0;
        } catch (const std::exception& e) {
            if (!is_backend_alive() || ++poll_failures >= max_control_failures) {
                return lost_job(e.what());
            }
        }
        if (cancel_failures >= max_control_failures) {
            return lost_job("could not deliver video cancellation");
        }
        if (std::chrono::steady_clock::now() >= cancel_deadline) {
            return lost_job("video cancellation did not settle within five minutes");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Cancel acceptance alone is not completion. Only a terminal state releases
    // this inference slot; a normal cancel never stops the backend process.
    if (canceled()) return canceled_result(true);
    const auto state = status.at("status").get<std::string>();
    if (state != "completed") {
        const auto detail = status.value("error", json(nullptr));
        return error("video job " + state + (detail.is_null() ? "" : ": " + detail.dump()));
    }
    try {
        const auto& resolved = status.at("resolved");
        const auto frames = resolved.at("frame_count").get<int>();
        const auto fps = resolved.at("fps").get<int>();
        const auto actual_format = resolved.at("output_format").get<std::string>();
        if (frames <= 0 || fps <= 0 || actual_format != format) {
            return error("invalid completed video metadata");
        }
        const auto mime = format == "mp4" ? "video/mp4" : "video/webm";
        // The job is already terminal. Canceling the one-shot content transfer
        // no longer requires a backend cancel or process stop.
        const auto clip = get(job_path + "/content", 120, RequestCancelToken{cancel, nullptr});
        if (canceled()) return canceled_result(true);
        const auto type = clip.headers.find("content-type");
        if (clip.status_code != 200 || clip.body.empty() ||
            type == clip.headers.end() || type->second != mime) {
            return error("could not retrieve encoded video (HTTP " + std::to_string(clip.status_code) + ")");
        }
        // Reuse Lemonade's existing video_store publishing path. Only metadata
        // and the clip filename are persisted in the public job record.
        return {{"b64_json", JsonUtils::base64_encode(clip.body)},
                {"mime_type", mime}, {"output_format", actual_format},
                {"frame_count", frames}, {"fps", fps}, {"duration", static_cast<double>(frames) / fps}};
    } catch (const std::exception& e) {
        return canceled() ? canceled_result(true) : error(std::string("video content failed: ") + e.what());
    }
}

std::string TheNoiseServer::upscale_via_cli(
    const std::string& b64_image,
    const std::string& upscale_model_path,
    double upscale_factor) {

    std::string configured_backend;
    if (recipe_options_.has_option("thenoise_backend")) {
        configured_backend = recipe_options_.get_option("thenoise_backend").get<std::string>();
    }
    if (configured_backend.empty()) {
        if (auto* cfg = RuntimeConfig::global()) {
            configured_backend = cfg->backend_string("thenoise", "backend");
        }
    }
    const std::string backend = resolve_thenoise_backend(configured_backend);
    RuntimeConfig::validate_backend_choice("thenoise", backend);
    backend_manager_->install_backend(thenoise::spec()->recipe, backend);
    std::string exe_path = BackendUtils::get_backend_binary_path(*thenoise::spec(), backend);

    if (!fs::exists(exe_path)) {
        LOG(ERROR, "TheNoise") << "thenoise binary not found at: " << exe_path << std::endl;
        return "";
    }

    std::string raw = JsonUtils::base64_decode(b64_image);

    fs::path runtime_base = path_from_utf8(get_runtime_dir());
    std::random_device rd;
    std::uniform_int_distribution<unsigned int> dis(0, 0xFFFFFF);

    fs::path temp_dir;
    std::error_code ec;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream suffix;
        suffix << "thenoise-upscale-" << nonce << "-" << std::hex << dis(rd);
        fs::path candidate = runtime_base / suffix.str();

        ec.clear();
        if (fs::create_directory(candidate, ec)) {
            temp_dir = candidate;
            break;
        }
    }

    if (temp_dir.empty()) {
        LOG(ERROR, "TheNoise") << "Failed to create temporary directory for upscale" << std::endl;
        return "";
    }

    fs::path input_path = temp_dir / "input.png";
    fs::path output_path = temp_dir / "output.png";

    struct TempFileGuard {
        fs::path path;
        bool recursive = false;
        ~TempFileGuard() {
            std::error_code ec;
            if (recursive) {
                fs::remove_all(path, ec);
            } else {
                fs::remove(path, ec);
            }
        }
    };
    TempFileGuard dir_guard{temp_dir, true};
    TempFileGuard input_guard{input_path};
    TempFileGuard output_guard{output_path};

    {
        std::ofstream out(input_path, std::ios::binary);
        out.write(raw.data(), raw.size());
    }

    std::vector<std::string> args = {
        "upscale",
        "--pixel-upscaler", upscale_model_path,
        "--input", input_path.string(),
        "--out", output_path.string()
    };
    if (upscale_factor > 0.0) {
        args.push_back("--upscale-factor");
        args.push_back(std::to_string(upscale_factor));
    }

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef _WIN32
    launch_bundled_python(exe_path, args, env_vars);
#endif
    const std::string selected_device = recipe_options_.has_option("thenoise_device")
        ? recipe_options_.get_option("thenoise_device").get<std::string>() : "";
    BackendUtils::apply_accelerator_device_env_vars(env_vars, "TheNoise", backend, selected_device);
    auto proc = ProcessManager::start_process(
        exe_path, args, "", true, false, env_vars);

    int exit_code = ProcessManager::wait_for_exit(proc, 300);
    if (exit_code == -1) {
        LOG(WARNING, "TheNoise") << "Upscale timed out, killing PID " << proc.pid << std::endl;
        ProcessManager::kill_process(proc);
    } else {
        // Closes the process handle; on POSIX the child is already reaped.
        ProcessManager::reap_process(proc);
    }

    std::string result;
    if (exit_code == 0 && fs::exists(output_path)) {
        std::ifstream in(output_path, std::ios::binary);
        std::string upscaled_data(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        result = JsonUtils::base64_encode(upscaled_data);
        LOG(INFO, "TheNoise") << "Upscale complete ("
            << raw.size() << " -> " << upscaled_data.size() << " bytes)" << std::endl;
    } else {
        LOG(WARNING, "TheNoise") << "Upscale failed (exit code: "
            << exit_code << ", model: " << upscale_model_path
            << ", cli: " << exe_path << ")" << std::endl;
    }

    return result;
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
