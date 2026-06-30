#include "lemon/backends/trellis/trellis_server.h"
#include "lemon/backends/trellis/trellis.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backend_manager.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
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

InstallParams TrellisServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    params.repo = "pwilkin/trellis.cpp";
    const std::string variant = (backend.rfind("rocm", 0) == 0) ? "rocm" : backend;
#ifdef _WIN32
    params.filename = "trellis-" + variant + "-windows-x64.zip";
#else
    params.filename = "trellis-" + variant + "-linux-x64.tar.gz";
#endif
    return params;
}

TrellisServer::TrellisServer(const std::string& log_level,
                             ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("trellis-server", log_level, model_manager, backend_manager) {}

TrellisServer::~TrellisServer() {
    unload();
}

std::string TrellisServer::backend_variant() const {
    if (auto* cfg = RuntimeConfig::global()) {
        const std::string section = RuntimeConfig::recipe_to_config_section(trellis::spec()->recipe);
        const std::string b = cfg->backend_string(section, "backend");
        if (!b.empty() && b != "auto") {
            return b;
        }
    }
    return "vulkan";
}

std::string TrellisServer::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = trellis::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

void TrellisServer::load(const std::string& model_name,
                         const ModelInfo& model_info,
                         const RecipeOptions& options,
                         bool do_not_upgrade) {
    (void)options;
    (void)do_not_upgrade;
    LOG(INFO, "trellis-server") << "Loading model: " << model_name << std::endl;

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

    // The checkpoint is the directory of TRELLIS.2 GGUFs.
    std::vector<std::string> args = {
        "--models", model_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    // Run the 512 cascade (TRELLIS_512=1): it skips the _1024 flow models and uses
    // smaller grids/meshes, so peak host RAM (~15 GB intermediates for the 1024
    // cascade) and VRAM drop substantially. The 1024 cascade can OOM a memory-
    // contended box; 512 is the safe default.
    std::vector<std::pair<std::string, std::string>> env_vars = {{"TRELLIS_512", "1"}};

    // ROCm: the slim binary finds the ROCm runtime in the shared TheRock SDK plus
    // its colocated ggml .so. Prepend the TheRock lib dir + the exe dir to the
    // loader path (mirrors sd-cpp / llama.cpp). Vulkan needs no special env.
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

    LOG(INFO, "trellis-server") << "Starting " << exe_path << " on port " << port_ << std::endl;
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path, args, "", is_debug(), false, env_vars);
    set_process_handle(started_handle);
    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start trellis-server process");
    }
    LOG(INFO, "trellis-server") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("trellis-server failed to start or become ready");
    }
}

void TrellisServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "trellis-server") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

void TrellisServer::model_3d_generations(const json& request, httplib::DataSink& sink) {
    if (!request.contains("image") || !request["image"].is_string()) {
        return;  // handler already validated; nothing to stream
    }
    std::string b64 = request["image"].get<std::string>();
    // Strip an optional data URL prefix ("data:image/png;base64,").
    auto comma = b64.find(',');
    if (b64.rfind("data:", 0) == 0 && comma != std::string::npos) {
        b64 = b64.substr(comma + 1);
    }
    std::string image = utils::JsonUtils::base64_decode(b64);

    std::vector<utils::MultipartField> fields;
    fields.push_back({"image", image, "input.png", "image/png"});
    if (request.contains("seed")) {
        fields.push_back({"seed", std::to_string(request["seed"].get<int>()), "", ""});
    }
    // Cascade resolution (512/1024/1536); trellis-server defaults to 512 if absent.
    if (request.contains("resolution")) {
        std::string res = request["resolution"].is_string()
                              ? request["resolution"].get<std::string>()
                              : std::to_string(request["resolution"].get<int>());
        fields.push_back({"resolution", res, "", ""});
    }
    // Background removal mode (threshold | birefnet); uploads default to birefnet
    // on the client so an arbitrary photo's real background gets matted out.
    if (request.contains("bg_removal") && request["bg_removal"].is_string()) {
        fields.push_back({"bg_removal", request["bg_removal"].get<std::string>(), "", ""});
    }

    // 3D reconstruction is slow (the 1024 cascade is minutes); allow ample time.
    // Errors write nothing — the handler turns an empty body into an HTTP error.
    try {
        auto resp = utils::HttpClient::post_multipart(get_base_url() + "/generate", fields, 1800);
        if (resp.curl_code != 0) {
            LOG(ERROR, "trellis-server") << "generation transport error: " << resp.curl_error << std::endl;
            return;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG(ERROR, "trellis-server") << "generation failed (HTTP " << resp.status_code
                                         << "): " << resp.body << std::endl;
            return;
        }
        sink.write(resp.body.data(), resp.body.size());
    } catch (const std::exception& e) {
        LOG(ERROR, "trellis-server") << "model_3d_generations failed: " << e.what() << std::endl;
    }
}

}  // namespace backends

namespace backends {

namespace {
// The checkpoint is a directory of TRELLIS.2 GGUFs; trellis-server scans it
// (--models). Resolve to the active Hugging Face snapshot directory rather than
// the cache root, so the resolved path actually contains the downloaded files.
class TrellisOps : public BackendOps {
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

namespace trellis {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<TrellisServer>(ctx);
}

const BackendSpec* spec() { return make_spec<TrellisServer>(descriptor); }
const BackendOps* ops() { return single_ops<TrellisOps>(); }

}  // namespace trellis
}  // namespace backends
}  // namespace lemon
