#include "lemon/backends/VTE/VTE_server.h"
#include "lemon/backends/VTE/VTE.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/gguf_reader.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

// Mirrors vte/compiler/sanitizer.py::SUPPORTED_ARCHITECTURES. Checked here
// too (VTE's own sanitizer is the real gate) so an unsupported model fails
// before spawning a subprocess, with a clearer error.
const std::set<std::string> kVteSupportedArchitectures = {"qwen2", "granite", "qwen35", "llama"};

int current_process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

}  // namespace

InstallParams VTEServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    // The framework calls this with the already-normalized backend name
    // (BackendManager::get_install_params resolves "rocm" -> "rocm-stable"
    // via normalize_backend_name before invoking this), not the raw "rocm"
    // a caller elsewhere in this file might pass into install_backend()/
    // get_backend_binary_path() (both of which do their own normalization).
    if (backend != "rocm-stable") {
        throw std::runtime_error("VTE backend '" + backend + "' is not supported. Supported: rocm");
    }

    // One portable bundle covers all of RDNA3 (gfx1100/1101/1102): VTE ships
    // precompiled kernels for every supported GPU family in the same wheel
    // and does its own runtime device detection, so no per-arch release tag.
    params.repo = "kyuubyN/VTE";
#ifdef _WIN32
    params.filename = "vte-server-" + version + "-windows-x64.zip";
#else
    throw std::runtime_error("VTE is Windows-only today (see docs/USAGE.md in the VTE repo).");
#endif
    return params;
}

VTEServer::VTEServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("vte-server", log_level, model_manager, backend_manager) {
}

VTEServer::~VTEServer() {
    unload();
}

void VTEServer::load(const std::string& model_name,
                      const ModelInfo& model_info,
                      const RecipeOptions& options,
                      bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "VTE") << "Loading model: " << model_name << std::endl;

    backend_manager_->install_backend(VTE::spec()->recipe, "rocm");

    std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty() || !fs::exists(fs::path(gguf_path))) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }

    const std::string architecture = model_info.gguf.architecture;
    if (!architecture.empty() && kVteSupportedArchitectures.count(architecture) == 0) {
        throw std::runtime_error(
            "VTE does not support GGUF architecture '" + architecture +
            "' (supported: qwen2, granite, qwen35, llama)."
        );
    }

    int ctx_size = options.get_option("ctx_size");
    context_length_ = ctx_size;

    port_ = choose_port();

    std::string executable = BackendUtils::get_backend_binary_path(*VTE::spec(), "rocm");
    LOG(INFO, "VTE") << "Using executable: " << executable << std::endl;

    std::vector<std::string> args = {
        "--gguf-path", gguf_path,
        "--port", std::to_string(port_),
        "--host", "127.0.0.1",
        "--context-length", std::to_string(ctx_size),
        // Lemonade owns lifetime and VRAM policy: disable vte-server's own idle
        // auto-unload (would race a long request) and its 80%-free-VRAM
        // preflight (would collide with the router's eviction-and-retry). Its
        // per-allocation OOM guard still protects a real out-of-memory load.
        "--idle-timeout", "0",
        "--vram-limit-pct", "0",
        // Windows' TerminateProcess delivers no catchable signal to a child,
        // even on the normal unload path -- this lets vte-server detect a
        // vanished parent and exit instead of surviving as an orphan.
        "--parent-pid", std::to_string(current_process_id()),
    };

    std::vector<std::pair<std::string, std::string>> env_vars;
    // Prevent system/user Python packages from leaking into the bundled environment.
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    // get_therock_lib_path() only returns non-empty when BackendManager already
    // judged the system ROCm absent or version-incompatible and installed TheRock
    // instead (see install_backend() above). Trying resolve_rocm_root() first would
    // let an incompatible-but-present system ROCm win over that decision.
    std::optional<fs::path> effective_root;
    std::optional<fs::path> effective_bin;
    std::string rocm_arch = SystemInfo::get_rocm_arch();
    if (!rocm_arch.empty()) {
        std::string therock_bin = BackendUtils::get_therock_lib_path(rocm_arch);
        if (!therock_bin.empty()) {
            fs::path therock_bin_path = fs::absolute(path_from_utf8(therock_bin));
            effective_bin = therock_bin_path;
            effective_root = therock_bin_path.parent_path();
        }
    }
    if (!effective_root) {
        if (auto system_root = BackendUtils::resolve_rocm_root()) {
            effective_root = *system_root;
            effective_bin = *system_root / "bin";
        }
    }
    if (effective_root && effective_bin) {
        std::string bin_path_str = path_to_utf8(*effective_bin);
        std::string root_path_str = path_to_utf8(*effective_root);
        std::string new_path = bin_path_str;
        const char* existing_path = std::getenv("PATH");
        if (existing_path && strlen(existing_path) > 0) {
            new_path += ";" + std::string(existing_path);
        }
        env_vars.push_back({"PATH", new_path});
        env_vars.push_back({"HIP_PATH", root_path_str});
        LOG(DEBUG, "VTE") << "Using ROCm runtime at " << root_path_str << " (bin: " << bin_path_str << ")" << std::endl;
    } else {
        LOG(WARNING, "VTE") << "No ROCm runtime resolved; vte-server will only start if a HIP SDK is already in PATH." << std::endl;
    }

    bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars));

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        context_length_ = 0;
        throw std::runtime_error("vte-server failed to start within timeout");
    }

    LOG(DEBUG, "VTE") << "Model loaded on port " << get_backend_port() << std::endl;
}

void VTEServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        ProcessManager::stop_process(handle);
    }
    context_length_ = 0;
}

namespace {

// vte-server itself now accepts these aliases directly too (belt and
// suspenders); normalizing here keeps VTEServer consistent with how
// llamacpp/vllm handle the same two fields before forward_request().
json normalize_vte_request(const json& request) {
    json normalized = JsonUtils::with_legacy_max_tokens_alias(request);
    if (normalized.contains("repeat_penalty") && !normalized.contains("repetition_penalty")) {
        normalized["repetition_penalty"] = normalized["repeat_penalty"];
    }
    return normalized;
}

}  // namespace

json VTEServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", normalize_vte_request(request));
}

json VTEServer::completion(const json& request) {
    return forward_request("/v1/completions", normalize_vte_request(request));
}

// default_backend_ops() does not read GGUF files, so without this VTE model
// listings never report max_context_window and model_info.gguf.architecture
// (checked in load() above) stays empty.
class VTEOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        const std::string gguf_path = info.resolved_path();
        if (gguf_path.size() < 5) {
            return;
        }
        std::string ext = gguf_path.substr(gguf_path.size() - 5);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".gguf") {
            return;
        }
        std::error_code ec;
        if (!std::filesystem::exists(lemon::utils::path_from_utf8(gguf_path), ec)) {
            return;
        }
        GgufMetadata meta;
        if (!read_gguf_metadata(meta, gguf_path)) {
            return;
        }
        info.max_context_window = meta.context_length;
        info.gguf = std::move(meta);
    }
};

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace VTE {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<VTEServer>(ctx);
}

const BackendSpec* spec() { return make_spec<VTEServer>(descriptor, /*split=*/false); }
const BackendOps* ops() { return single_ops<VTEOps>(); }

}  // namespace VTE
}  // namespace backends
}  // namespace lemon
