// NOTE: this file has NOT been compiled in the session that wrote it (no
// cmake/MSVC toolchain was available). It is grounded line-by-line against
// vllm_server.cpp and moonshine_server.cpp (the closest existing precedents:
// vLLM is the other Python-based backend, Moonshine the simplest
// single-flavor/non-selectable one) -- build and run this against a real
// lemond before merging.

#include "lemon/backends/VTE/VTE_server.h"
#include "lemon/backends/VTE/VTE.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/model_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
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

// GGUF architectures VTE can actually load today (vte/compiler/sanitizer.py::
// SUPPORTED_ARCHITECTURES, ground truth in the VTE repo). Checking this here,
// before the subprocess is even spawned, turns an unsupported model into a
// clear error message instead of a subprocess that starts and then fails
// deep inside VTEModel._load() -- VTE's own sanitizer.validate() is the real
// gate either way, this is defense in depth for a better error surface only.
const std::set<std::string> kVteSupportedArchitectures = {"qwen2", "granite", "qwen35"};

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
    if (backend != "rocm") {
        throw std::runtime_error("VTE backend '" + backend + "' is not supported. Supported: rocm");
    }

    // Hosted under the VTE project's own GitHub repo (not lemonade-sdk) --
    // deliberate choice for the first integration, see the integration plan.
    // One portable bundle covers all of RDNA3 (gfx1100/1101/1102): unlike
    // vLLM, VTE ships its precompiled kernels for every supported GPU family
    // inside the same wheel and does its own runtime device detection, so no
    // per-GPU-arch release tag is needed here.
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
            "' (supported: qwen2, granite, qwen35)."
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
        // Watchdog for the "lemond died without calling unload() on anyone"
        // case -- Windows' TerminateProcess (what stop_process() uses even
        // on the NORMAL unload path, confirmed by reading process_windows.cpp)
        // delivers no catchable signal to this subprocess at all, so this is
        // the mechanism that actually prevents vte-server from surviving as
        // an orphan holding VRAM if the parent vanishes uncleanly. VRAM
        // itself is not at risk on any *normal* termination of this process
        // (graceful or hard-killed) -- the GPU driver reclaims all VRAM tied
        // to a process on its exit, same as it would for any other GPU
        // application; this watchdog is purely about not leaving a live,
        // still-serving orphan process behind.
        "--parent-pid", std::to_string(current_process_id()),
    };

    std::vector<std::pair<std::string, std::string>> env_vars;
    // Prevent system/user Python packages from leaking into the bundled environment.
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

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

json VTEServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json VTEServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace VTE {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<VTEServer>(ctx);
}

const BackendSpec* spec() { return make_spec<VTEServer>(descriptor, /*split=*/false); }
const BackendOps* ops() { return default_backend_ops(); }

}  // namespace VTE
}  // namespace backends
}  // namespace lemon
