#include "lemon/backends/acestep/acestep_server.h"
#include "lemon/backends/acestep/acestep.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

namespace {
// ace-server returns synth results as multipart/mixed (audio part + latent
// part). Extract the first audio part's raw bytes. Returns "" if not found.
std::string extract_multipart_audio(const std::string& body) {
    const std::string ct = "Content-Type: audio/";
    size_t hpos = body.find(ct);
    if (hpos == std::string::npos) return "";
    size_t bstart = body.find("\r\n\r\n", hpos);
    if (bstart == std::string::npos) return "";
    bstart += 4;
    size_t bend = body.find("\r\n--ace-batch-boundary", bstart);
    if (bend == std::string::npos) return "";
    return body.substr(bstart, bend - bstart);
}

bool is_instrumental_sentinel(const std::string& lyrics) {
    size_t b = lyrics.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return true;
    size_t e = lyrics.find_last_not_of(" \t\r\n");
    std::string t = lyrics.substr(b, e - b + 1);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return t == "[instrumental]";
}
}  // namespace

InstallParams AceStepServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    params.repo = "pwilkin/acestep.cpp";
    const std::string variant = (backend.rfind("rocm", 0) == 0) ? "rocm" : backend;
#ifdef _WIN32
    params.filename = "acestep-" + variant + "-windows-x64.zip";
#else
    params.filename = "acestep-" + variant + "-linux-x64.tar.gz";
#endif
    return params;
}

AceStepServer::AceStepServer(const std::string& log_level,
                             ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("acestep-server", log_level, model_manager, backend_manager) {}

AceStepServer::~AceStepServer() {
    unload();
}

std::string AceStepServer::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = acestep::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

void AceStepServer::load(const std::string& model_name,
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool /*do_not_upgrade*/) {
    LOG(INFO, "acestep-server") << "Loading model: " << model_name << std::endl;

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
        throw std::runtime_error("Model path not found for checkpoint: " + model_info.checkpoint());
    }

    std::string backend = options.get_option("acestep_backend");
    if (backend.empty()) {
        auto supported = SystemInfo::get_supported_backends("acestep");
        if (supported.backends.empty()) {
            throw UnsupportedOperationException(
                "ACE-Step audio generation", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected");
        }
        backend = supported.backends[0];
    }
    RuntimeConfig::validate_backend_choice("acestep", backend);
    const std::string exe_path = resolve_binary_path(backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    // The checkpoint is the directory of ACE-Step GGUFs; ace-server scans it by
    // architecture. --keep-loaded keeps the model resident (hot) across requests.
    std::vector<std::string> args = {
        "--models", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
        "--keep-loaded",
    };

    // ROCm/CUDA: the slim binary finds its runtime in the shared TheRock SDK
    // (ROCm) or bundled next to the exe (CUDA), plus the colocated ggml library.
    // Prepend those dirs to the loader path (mirrors sd-cpp / llama.cpp). Vulkan
    // needs no special env.
    std::vector<std::pair<std::string, std::string>> env_vars;
    const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
    auto prepend_loader_path = [&env_vars, &exe_dir](const std::string& extra_dirs) {
#ifdef _WIN32
        std::string path = extra_dirs.empty() ? exe_dir : (extra_dirs + ";" + exe_dir);
        if (const char* p = std::getenv("PATH")) path += std::string(";") + p;
        env_vars.push_back({"PATH", path});
#else
        std::string ld = extra_dirs.empty() ? exe_dir : (extra_dirs + ":" + exe_dir);
        if (const char* p = std::getenv("LD_LIBRARY_PATH")) ld += std::string(":") + p;
        env_vars.push_back({"LD_LIBRARY_PATH", ld});
#endif
    };
    if (backend == "rocm") {
        const std::string arch = SystemInfo::get_rocm_arch();
        const std::string therock_lib = arch.empty() ? "" : BackendUtils::get_therock_lib_path(arch);
        std::string dirs;
        if (!therock_lib.empty()) {
#ifdef _WIN32
            const std::string llvm_bin =
                (std::filesystem::path(therock_lib).parent_path() / "lib" / "llvm" / "bin").string();
            dirs = therock_lib + ";" + llvm_bin;
#else
            dirs = therock_lib + ":" + therock_lib + "/llvm/lib";
#endif
        }
        prepend_loader_path(dirs);
    } else if (backend == "cuda") {
        prepend_loader_path("");
        BackendUtils::apply_cuda_env_vars(env_vars, "acestep-server");
    }

    LOG(INFO, "acestep-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path, args, "", is_debug(), false, env_vars);
    set_process_handle(started_handle);
    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start acestep-server process");
    }
    LOG(INFO, "acestep-server") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("acestep-server failed to start or become ready");
    }
}

void AceStepServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "acestep-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

bool AceStepServer::run_job(const std::string& path, const std::string& body,
                            std::string& result, std::string& error) {
    const std::string stage = path.substr(1);
    const std::string base = get_base_url();
    auto submit = utils::HttpClient::post(base + path, body,
                                          {{"Content-Type", "application/json"}}, 60);
    if (submit.status_code != 200) {
        LOG(ERROR, "acestep-server") << stage << " submit failed (HTTP " << submit.status_code
                                     << "): " << submit.body << std::endl;
        error = stage + " submit failed (HTTP " + std::to_string(submit.status_code) + ")";
        return false;
    }
    std::string job_id = json::parse(submit.body).value("id", std::string());
    if (job_id.empty()) {
        LOG(ERROR, "acestep-server") << stage << " submit returned no job id: " << submit.body << std::endl;
        error = stage + " submit returned no job id";
        return false;
    }

    const std::string job_url = base + "/job?id=" + job_id;
    std::string status;
    for (int i = 0; i < 1200; ++i) {
        auto poll = utils::HttpClient::get(job_url, {}, 30,
                                           utils::HttpSecurityPolicy::TrustedLoopback);
        if (poll.status_code == 200) {
            try { status = json::parse(poll.body).value("status", std::string()); }
            catch (...) { status.clear(); }
        }
        if (status == "done" || status == "failed" || status == "cancelled") break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (status != "done") {
        LOG(ERROR, "acestep-server") << stage << " job " << job_id << " ended with status '"
                                     << status << "'" << std::endl;
        error = stage + " job ended with status '" + status + "'";
        return false;
    }

    auto fetched = utils::HttpClient::get(job_url + "&result=1", {}, 120,
                                          utils::HttpSecurityPolicy::TrustedLoopback);
    if (fetched.status_code != 200) {
        LOG(ERROR, "acestep-server") << "fetching " << stage << " job result failed (HTTP "
                                     << fetched.status_code << ")" << std::endl;
        error = "fetching " + stage + " job result failed (HTTP " +
                std::to_string(fetched.status_code) + ")";
        return false;
    }
    result = std::move(fetched.body);
    return true;
}

void AceStepServer::audio_generations(const json& request, httplib::DataSink& sink) {
    auto fail = [&sink](const std::string& message) {
        const std::string payload =
            json{{"error", {{"message", message}, {"type", "backend_error"}}}}.dump();
        sink.write(payload.data(), payload.size());
    };
    try {
        json synth;
        synth["caption"] = request.value("prompt", std::string());
        synth["synth_model"] = "";
        synth["output_format"] = "wav24";
        if (request.contains("duration")) synth["duration"] = request["duration"];
        if (request.contains("seed"))     synth["seed"]     = request["seed"];
        if (request.contains("steps"))    synth["inference_steps"] = request["steps"];

        std::string lyrics;
        if (request.contains("lyrics") && request["lyrics"].is_string()) {
            lyrics = request["lyrics"].get<std::string>();
        }
        const bool vocal = !is_instrumental_sentinel(lyrics);

        std::string error;
        std::string synth_body;
        if (vocal) {
            synth["lyrics"] = lyrics;
            synth["lm_mode"] = "generate";
            std::string language = "en";
            if (request.contains("vocal_language") && request["vocal_language"].is_string()) {
                language = request["vocal_language"].get<std::string>();
            }
            synth["vocal_language"] = language;

            if (!run_job("/lm", synth.dump(), synth_body, error)) {
                fail(error);
                return;
            }
        } else {
            synth["lyrics"] = "[Instrumental]";
            synth_body = synth.dump();
        }

        std::string result;
        if (!run_job("/synth", synth_body, result, error)) {
            fail(error);
            return;
        }
        std::string audio = extract_multipart_audio(result);
        if (audio.empty()) {
            LOG(ERROR, "acestep-server") << "no audio part in multipart result" << std::endl;
            fail("no audio part in synth result");
            return;
        }
        sink.write(audio.data(), audio.size());
    } catch (const std::exception& e) {
        LOG(ERROR, "acestep-server") << "audio_generations failed: " << e.what() << std::endl;
        fail(e.what());
    }
}

}  // namespace backends

namespace backends {

namespace {
// ACE-Step's HF repo holds many variants; the checkpoint variant names the DiT,
// and we additionally fetch one LM, the text encoder, and the VAE. Resolves to
// the active snapshot directory (ace-server scans --models).
class AceStepOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        (void)info;
        std::filesystem::path root = lemon::utils::path_from_utf8(ctx.model_cache_path);
        std::filesystem::path snap = hf_cache::active_snapshot_path(root);
        if (!snap.empty() && hf_cache::exists(snap)) {
            return lemon::utils::path_to_utf8(snap);
        }
        return ctx.model_cache_path;
    }

    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        static const std::vector<std::string> kCompanions = {
            "acestep-5Hz-lm-4B-Q8_0.gguf",     // language model (vocals/auto-lyrics)
            "Qwen3-Embedding-0.6B-BF16.gguf",  // text encoder
            "vae-BF16.gguf",                   // VAE
        };
        std::vector<std::string> want = {main_variant};
        for (const auto& c : kCompanions) {
            if (std::find(repo_files.begin(), repo_files.end(), c) != repo_files.end()) {
                want.push_back(c);
            }
        }
        return want;
    }
};
}  // namespace

namespace acestep {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AceStepServer>(ctx);
}

const BackendSpec* spec() { return make_spec<AceStepServer>(descriptor); }
const BackendOps* ops() { return single_ops<AceStepOps>(); }

}  // namespace acestep
}  // namespace backends
}  // namespace lemon
