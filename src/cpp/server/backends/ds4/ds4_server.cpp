#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/model_manager.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <filesystem>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// ds4 has no managed install: the only supported variant is "system" (a
// locally built binary on PATH), and install_backend() early-returns for it.
InstallParams Ds4Server::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    throw std::runtime_error("ds4 backend '" + backend +
                             "' has no managed install; build ds4-server locally and put it on PATH");
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
    (void)do_not_upgrade;  // no install/upgrade machinery: locally built binary

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

    std::string executable = find_executable_in_path(ds4::descriptor.binary);
    if (executable.empty()) {
        throw std::runtime_error(
            "ds4-server binary not found on PATH. Build it from https://github.com/antirez/ds4 "
            "(make strix-halo | make cuda | make metal) and place ds4-server on PATH.");
    }

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
    if (!ds4_args.empty()) {
        std::vector<std::string> custom_args = parse_custom_args(ds4_args);
        args.insert(args.end(), custom_args.begin(), custom_args.end());
    }

    LOG(INFO, "DS4") << "Starting ds4-server (" << executable << ") for " << gguf_path
                     << " on port " << port_ << std::endl;

    bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    set_process_handle(ProcessManager::start_process(executable, args, "", inherit_output, true));

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
