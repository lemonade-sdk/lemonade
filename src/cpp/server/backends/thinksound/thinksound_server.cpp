#include "lemon/backends/thinksound/thinksound_server.h"
#include "lemon/backends/thinksound/thinksound.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backend_manager.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

InstallParams ThinkSoundServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    params.repo = "pwilkin/thinksound.cpp";
    const std::string variant = (backend.rfind("rocm", 0) == 0) ? "rocm" : backend;
#ifdef _WIN32
    params.filename = "thinksound-" + variant + "-windows-x64.zip";
#else
    params.filename = "thinksound-" + variant + "-linux-x64.tar.gz";
#endif
    return params;
}

ThinkSoundServer::ThinkSoundServer(const std::string& log_level,
                                   ModelManager* model_manager,
                                   BackendManager* backend_manager)
    : WrappedServer("thinksound-server", log_level, model_manager, backend_manager) {}

ThinkSoundServer::~ThinkSoundServer() {
    unload();
}

std::string ThinkSoundServer::backend_variant() const {
    if (auto* cfg = RuntimeConfig::global()) {
        const std::string section = RuntimeConfig::recipe_to_config_section(thinksound::spec()->recipe);
        const std::string b = cfg->backend_string(section, "backend");
        if (!b.empty() && b != "auto") {
            return b;
        }
    }
    return "vulkan";
}

std::string ThinkSoundServer::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = thinksound::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

void ThinkSoundServer::load(const std::string& model_name,
                            const ModelInfo& model_info,
                            const RecipeOptions& options,
                            bool do_not_upgrade) {
    (void)options;
    (void)do_not_upgrade;
    LOG(INFO, "thinksound-server") << "Loading model: " << model_name << std::endl;

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !std::filesystem::exists(model_path)) {
        throw std::runtime_error("Model path not found for checkpoint: " + model_info.checkpoint());
    }

    const std::string backend = backend_variant();
    const std::string exe_path = resolve_binary_path(backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    // The checkpoint is the directory of ThinkSound GGUFs; ts-server resolves the
    // individual networks (dit/t5/clip/vae/tokenizers) within it.
    std::vector<std::string> args = {
        "--dir", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    // ROCm: the slim binary finds the ROCm runtime in the shared TheRock SDK plus
    // its colocated ggml .so. Prepend the TheRock lib dir + the exe dir to the
    // loader path (mirrors sd-cpp / llama.cpp). Vulkan needs no special env.
    std::vector<std::pair<std::string, std::string>> env_vars;
    if (backend == "rocm") {
        const std::string arch = SystemInfo::get_rocm_arch();
        const std::string therock_lib = arch.empty() ? "" : BackendUtils::get_therock_lib_path(arch);
        const std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
#ifdef _WIN32
        std::string path = therock_lib.empty() ? exe_dir : (therock_lib + ";" + exe_dir);
        if (const char* p = std::getenv("PATH")) path += std::string(";") + p;
        env_vars.push_back({"PATH", path});
#else
        std::string ld = therock_lib.empty() ? exe_dir : (therock_lib + ":" + exe_dir);
        if (const char* p = std::getenv("LD_LIBRARY_PATH")) ld += std::string(":") + p;
        env_vars.push_back({"LD_LIBRARY_PATH", ld});
#endif
    }

    LOG(INFO, "thinksound-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path, args, "", is_debug(), false, env_vars);
    set_process_handle(started_handle);
    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start thinksound-server process");
    }
    LOG(INFO, "thinksound-server") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("thinksound-server failed to start or become ready");
    }
}

void ThinkSoundServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "thinksound-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

void ThinkSoundServer::audio_generations(const json& request, httplib::DataSink& sink) {
    // Map the Lemonade /audio/generations request onto ts-server's /generate, then
    // stream the wav bytes back. Errors write nothing — the handler turns an empty
    // body into an HTTP error.
    json body;
    body["caption"] = request.value("prompt", std::string());
    body["description"] = request.contains("description") ? request["description"]
                        : request.contains("cot") ? request["cot"]
                        : body["caption"];
    if (request.contains("duration")) body["duration"] = request["duration"];
    if (request.contains("steps"))    body["steps"]    = request["steps"];
    if (request.contains("cfg"))      body["cfg"]      = request["cfg"];
    if (request.contains("seed"))     body["seed"]     = request["seed"];
    forward_streaming_request("/generate", body.dump(), sink, /*sse=*/false, /*timeout_seconds=*/600);
}

}  // namespace backends

namespace backends {

namespace {
// The checkpoint is a directory of ThinkSound GGUFs; resolve to the active
// Hugging Face snapshot directory (ts-server scans it via --dir).
class ThinkSoundOps : public BackendOps {
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
};
}  // namespace

namespace thinksound {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<ThinkSoundServer>(ctx);
}

const BackendSpec* spec() { return make_spec<ThinkSoundServer>(descriptor); }
const BackendOps* ops() { return single_ops<ThinkSoundOps>(); }

}  // namespace thinksound
}  // namespace backends
}  // namespace lemon
