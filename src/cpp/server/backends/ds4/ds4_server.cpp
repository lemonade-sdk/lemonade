#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// Upstream ds4 publishes no binaries, no releases and no tags, so ROCm builds
// come from lemonade-sdk/ds4-rocm, which builds a pinned upstream commit and
// bundles the ROCm runtime.
InstallParams Ds4Server::get_install_params(const std::string& backend, const std::string& version) {
    if (backend != "rocm") {
        throw std::runtime_error("ds4 backend '" + backend +
                                 "' is not supported. Supported: rocm");
    }

    // One archive per GPU target under a single release tag, named so it is
    // derivable from the tag alone. Check the architecture is one we publish
    // before building a name from it: model filtering normally keeps
    // unsupported hosts away, but the install path does not depend on that
    // having run, and an unchecked arch resolves to an asset that 404s.
    const std::string current_os = get_current_os();
    if (!ds4::publishes_for_os(current_os)) {
        throw std::runtime_error(
            "ds4 backend 'rocm' publishes no build for " + current_os + "; Linux only");
    }

    const std::string target_arch = SystemInfo::get_rocm_arch();
    if (target_arch.empty()) {
        throw std::runtime_error(
            "ds4 backend 'rocm' requires a ROCm GPU, but no ROCm architecture was detected");
    }
    if (!SystemInfo::backend_supports_arch("ds4", "rocm", target_arch)) {
        throw std::runtime_error(
            "ds4 backend 'rocm' publishes no build for " + target_arch +
            "; only gfx1151 is validated upstream");
    }

    InstallParams params;
    params.repo = "lemonade-sdk/ds4-rocm";
    params.filename = "ds4-" + version + "-linux-rocm-" + target_arch + "-x64.tar.gz";
    return params;
}

Ds4Server::Ds4Server(const std::string& log_level, ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("ds4-server", log_level, model_manager, backend_manager) {
}

Ds4Server::~Ds4Server() {
    unload();
}

void Ds4Server::load(const std::string& model_name, const ModelInfo& model_info,
                     const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pin is present

    std::string ds4_args = options.get_option("ds4_args");
    int ctx_size = options.get_option("ctx_size");

    // ds4-server only runs its own curated GGUFs. Accept either a
    // Hugging-Face-resolved local path or an absolute path used directly as
    // the checkpoint (user_models.json registrations of an existing file).
    std::string gguf_path = model_info.resolved_path("main");
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        const std::string checkpoint = model_info.checkpoint();
        if (!checkpoint.empty() && fs::path(checkpoint).is_absolute() && fs::exists(checkpoint)) {
            gguf_path = checkpoint;
        }
    }
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        throw std::runtime_error("ds4: no local GGUF found for model '" + model_name +
                                 "' (checkpoint: " + model_info.checkpoint() + ")");
    }

    // rocm is the only published variant, so there is no backend option to
    // read; ds4_args cannot select one either (see reserved_custom_arg_flags).
    const std::string backend = "rocm";
    device_type_ = DEVICE_GPU;

    backend_manager_->install_backend(ds4::spec()->recipe, backend);
    const std::string executable = BackendUtils::get_backend_binary_path(*ds4::spec(), backend);

    port_ = choose_port();

    std::vector<std::string> args;
    args.push_back("-m");
    args.push_back(gguf_path);
    args.push_back("--host");
    args.push_back("127.0.0.1");
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    if (ctx_size > 0) {
        args.push_back("-c");
        args.push_back(std::to_string(ctx_size));
    }

    // ds4-server defaults to full residency, which maps the entire model into
    // the ROCm arena. The only published ds4 model is an 81 GB DeepSeek V4 MoE,
    // and the only supported device (gfx1151) tops out around a 64 GB VRAM
    // carveout with a smaller usable arena, so full residency always OOMs
    // mid-load. Stream experts from disk instead; a user-supplied later flag
    // (e.g. --ssd-streaming-cache-experts) still wins since ds4-server parses
    // left-to-right.
    args.push_back("--ssd-streaming");

    if (!ds4_args.empty()) {
        const std::string validation_error =
            validate_custom_args(ds4_args, ds4::reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ds4-server arguments:\n" + validation_error);
        }
        LOG(DEBUG, "DS4") << "Adding custom arguments: " << ds4_args << std::endl;
        std::vector<std::string> custom_args = parse_custom_args(ds4_args);
        args.insert(args.end(), custom_args.begin(), custom_args.end());
    }

    // The managed bundle ships its own ROCm runtime next to the binary, which
    // has no RPATH. Without this the loader falls back to a system ROCm install
    // — working by accident on a developer box and failing on the clean hosts
    // the bundle exists to support.
    std::vector<std::pair<std::string, std::string>> env_vars;
    const std::string exe_dir = fs::path(executable).parent_path().string();
    std::string ld_path = exe_dir;
    if (const char* existing = std::getenv("LD_LIBRARY_PATH")) {
        ld_path += std::string(":") + existing;
    }
    env_vars.push_back({"LD_LIBRARY_PATH", ld_path});

    LOG(INFO, "DS4") << "Starting ds4-server (" << executable << ") for " << gguf_path
                     << " on port " << port_ << std::endl;

    bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    set_process_handle(
        ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars),
        executable, args);

    // ds4-server binds its port only after the model is fully loaded, so first
    // reachability means ready. There is no /health endpoint; /v1/models is the
    // cheapest always-on route and doubles as the watchdog probe.
    if (!wait_for_ready("/v1/models", HttpClient::get_default_timeout())) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("ds4-server failed to start within timeout");
    }
}

void Ds4Server::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "DS4") << "Stopping ds4-server" << std::endl;
        ProcessManager::stop_process(handle);
    }
}

json Ds4Server::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json Ds4Server::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json Ds4Server::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

namespace ds4 {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<Ds4Server>(ctx);
}

const BackendSpec* spec() {
    return make_spec<Ds4Server>(descriptor);
}

const BackendOps* ops() {
    return default_backend_ops();
}

}  // namespace ds4

}  // namespace backends
}  // namespace lemon
