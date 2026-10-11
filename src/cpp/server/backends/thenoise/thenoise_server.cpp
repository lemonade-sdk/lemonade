#include "lemon/backends/thenoise/thenoise_server.h"
#include "lemon/backends/thenoise/thenoise.h"
#include "lemon/backends/thenoise/thenoise_bundle.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <utility>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

std::optional<std::pair<int, int>> image_dimensions_from_bytes(const std::string& bytes) {
    auto at = [&bytes](size_t i) -> std::uint8_t {
        return static_cast<std::uint8_t>(bytes[i]);
    };
    if (bytes.size() >= 24 &&
        bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        bytes.compare(12, 4, "IHDR", 4) == 0) {
        auto be32 = [&](size_t i) -> std::uint32_t {
            return (std::uint32_t(at(i)) << 24) | (std::uint32_t(at(i + 1)) << 16) |
                   (std::uint32_t(at(i + 2)) << 8) | std::uint32_t(at(i + 3));
        };
        const auto width = be32(16);
        const auto height = be32(20);
        if (width > 0 && height > 0 &&
            width <= static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
            height <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return std::pair<int, int>{static_cast<int>(width), static_cast<int>(height)};
        }
    }
    if (bytes.size() >= 4 && at(0) == 0xff && at(1) == 0xd8) {
        size_t pos = 2;
        while (pos < bytes.size()) {
            if (at(pos) != 0xff) break;
            while (pos < bytes.size() && at(pos) == 0xff) ++pos;
            if (pos >= bytes.size()) break;
            const auto marker = at(pos++);
            if (marker == 0xd9 || marker == 0xda) break;
            if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
            if (bytes.size() - pos < 2) break;
            const size_t segment_len = (size_t(at(pos)) << 8) | at(pos + 1);
            if (segment_len < 2 || segment_len > bytes.size() - pos) break;
            const bool sof = marker >= 0xc0 && marker <= 0xcf &&
                             marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
            if (sof && segment_len >= 7) {
                const int height = (int(at(pos + 3)) << 8) | at(pos + 4);
                const int width = (int(at(pos + 5)) << 8) | at(pos + 6);
                if (width > 0 && height > 0) return std::pair<int, int>{width, height};
            }
            pos += segment_len;
        }
    }
    return std::nullopt;
}

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
    if (backend != "rocm") {
        throw std::runtime_error("TheNoise backend '" + backend + "' is not supported. Supported: rocm");
    }

    std::string target_arch = SystemInfo::rocm_asset_family(SystemInfo::get_rocm_arch());

    InstallParams params;
    params.repo = "lemonade-sdk/thenoise";
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

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef _WIN32
    launch_bundled_python(exe_path, args, env_vars);
    LOG(DEBUG, "TheNoise") << "Launching bundled interpreter: " << exe_path << std::endl;
#endif

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

json TheNoiseServer::image_edits(const json& request) {
    int n = request.value("n", 1);
    if (n < 1) n = 1;

    json data = json::array();
    for (int i = 0; i < n; ++i) {
        json body = build_request(request);
        // Unlike sd.cpp, TheNoise's edit() uses a 1024 px long-edge default
        // when size is absent. So derive real original image dimensions here.
        if (!request.contains("size") &&
            !(request.contains("width") && request.contains("height"))) {
            body.erase("width");
            body.erase("height");
            std::string first_reference;
            if (request.contains("image_files") && request["image_files"].is_array() &&
                !request["image_files"].empty()) {
                first_reference = request["image_files"][0].at("data").get<std::string>();
            } else if (request.contains("image_data") && request["image_data"].is_string()) {
                first_reference = request["image_data"].get<std::string>();
            }
            if (!first_reference.empty()) {
                const auto original_size = image_dimensions_from_bytes(
                    JsonUtils::base64_decode(first_reference));
                if (original_size) {
                    body["width"] = original_size->first;
                    body["height"] = original_size->second;
                } else {
                    LOG(WARNING, "TheNoise") << "Cannot determine the source image size; "
                          "the backend may use its default output resolution" << std::endl;
                }
            }
        }
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

std::string TheNoiseServer::upscale_via_cli(
    const std::string& b64_image,
    const std::string& upscale_model_path) {

    //ROCm is currently the only available backend. If other backends are added this needs to be parameterized accordingly.
    std::string exe_path = BackendUtils::get_backend_binary_path(*thenoise::spec(), "rocm");

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

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifdef _WIN32
    launch_bundled_python(exe_path, args, env_vars);
#endif
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
