#include "lemon/backends/llamacpp_router.h"
#include "lemon/backends/llamacpp_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/system_info.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kRouterSentinelRecipe = "llamacpp-router";
constexpr const char* kLogTag = "LlamaCppRouter";

// Mirror of the LlamaCppServer helper so our argv builder can pull defaults
// from the same rocm_channel setting. Keeping a small local copy avoids
// linker friction around exposing LlamaCppServer internals as a public API.
std::string resolve_llamacpp_backend_name(const std::string& backend) {
    if (backend != "rocm") return backend;
    std::string channel = "preview";
    if (auto* cfg = RuntimeConfig::global()) {
        channel = cfg->rocm_channel();
    }
    return "rocm-" + channel;
}

bool is_rocm_backend(const std::string& backend) {
    return backend == "rocm-stable" || backend == "rocm-preview" ||
           backend == "rocm-nightly";
}

} // namespace

LlamaCppRouter::LlamaCppRouter(const std::string& log_level,
                               ModelManager* model_manager,
                               BackendManager* backend_manager)
    : WrappedServer("llama-server (router)", log_level,
                    model_manager, backend_manager) {
    // The router is an LLM-shaped server that happens to host many models; we
    // treat its device_type_ as GPU by default and let resolve_backend_choice
    // downgrade it to CPU if the chosen backend ends up being "cpu".
    model_type_ = ModelType::LLM;
    device_type_ = DEVICE_GPU;
}

LlamaCppRouter::~LlamaCppRouter() {
    unload();
}

std::string LlamaCppRouter::resolve_backend_choice() const {
    std::string raw = "auto";
    if (auto* cfg = RuntimeConfig::global()) {
        raw = cfg->backend_string("llamacpp", "backend");
        if (raw.empty()) raw = "auto";
    }

    if (raw == "auto") {
        // Defer the concrete choice to SystemInfo, mirroring how
        // LlamaCppServer treats llamacpp_backend == "" / "auto".
        auto info = SystemInfo::get_supported_backends("llamacpp");
        if (!info.backends.empty()) {
            raw = info.backends.front();
        } else {
            raw = "cpu";
        }
    }

    return resolve_llamacpp_backend_name(raw);
}

std::vector<std::string> LlamaCppRouter::build_args(
    const std::string& llamacpp_backend) const {
    auto* cfg = RuntimeConfig::global();
    if (!cfg) {
        throw std::runtime_error(
            "LlamaCppRouter::build_args: RuntimeConfig is not set");
    }

    const std::string preset = cfg->router_models_preset();
    const std::string models_dir = cfg->router_models_dir();
    const std::string default_args = cfg->router_default_args();

    if (preset.empty() && models_dir.empty()) {
        throw std::runtime_error(
            "Router mode enabled but neither --models-preset nor --models-dir "
            "is configured. Set router_models_preset or router_models_dir "
            "in config.json or via the CLI.");
    }

    if (!preset.empty()) {
        const fs::path preset_path = utils::path_from_utf8(preset);
        if (!fs::exists(preset_path)) {
            throw std::runtime_error(
                "Router mode is enabled but --models-preset points to a "
                "missing file: " + utils::path_to_utf8(preset_path));
        }
        if (!fs::is_regular_file(preset_path)) {
            throw std::runtime_error(
                "Router mode is enabled but --models-preset is not a file: " +
                utils::path_to_utf8(preset_path));
        }
    } else {
        const fs::path models_dir_path = utils::path_from_utf8(models_dir);
        if (!fs::exists(models_dir_path)) {
            throw std::runtime_error(
                "Router mode is enabled but --models-dir points to a missing "
                "directory: " + utils::path_to_utf8(models_dir_path));
        }
        if (!fs::is_directory(models_dir_path)) {
            throw std::runtime_error(
                "Router mode is enabled but --models-dir is not a directory: " +
                utils::path_to_utf8(models_dir_path));
        }
    }

    std::vector<std::string> args;
    std::set<std::string> reserved;

    auto push = [&](const std::string& k,
                    const std::vector<std::string>& aliases = {}) {
        args.push_back(k);
        reserved.insert(k);
        reserved.insert(aliases.begin(), aliases.end());
    };
    auto push_val = [&](const std::string& k, const std::string& v,
                        const std::vector<std::string>& aliases = {}) {
        args.push_back(k);
        args.push_back(v);
        reserved.insert(k);
        reserved.insert(aliases.begin(), aliases.end());
    };

    // Router source. Prefer --models-preset when both are set (the preset file
    // may reference per-model aliases / args the directory walk cannot).
    if (!preset.empty()) {
        push_val("--models-preset", preset);
    } else {
        push_val("--models-dir", models_dir);
    }

    // Host/port: bind to loopback; pick a free port like other backends do.
    push_val("--host", "127.0.0.1");
    push_val("--port", std::to_string(port_));

    // Default chat template rendering so tool-calling + thinking tokens work
    // with the same surface area LlamaCppServer exposes.
    push("--jinja", std::vector<std::string>{"--no-jinja"});

    // GPU layer offload: 99 for non-cpu backends, 0 for cpu.
    const bool use_gpu = (llamacpp_backend != "cpu");
    push_val("-ngl", use_gpu ? "99" : "0",
             std::vector<std::string>{"--gpu-layers", "--n-gpu-layers"});

    // Disable the web UI by default — matches LlamaCppServer.
    if (default_args.find("--no-webui") == std::string::npos) {
        args.push_back("--no-webui");
    }

    // Disable mmap on iGPU systems — matches LlamaCppServer.
    if (SystemInfo::get_has_igpu() &&
        default_args.find("--no-mmap") == std::string::npos) {
        args.push_back("--no-mmap");
    }

    // Finally, append the user-supplied default args (after validation) so
    // they can override any of the above flags the user needs to customize.
    if (!default_args.empty()) {
        std::string err =
            utils::validate_custom_args(default_args, reserved);
        if (!err.empty()) {
            throw std::invalid_argument(
                "Invalid router_default_args for llama-server:\n" + err);
        }
        auto extra = utils::parse_custom_args(default_args);
        args.insert(args.end(), extra.begin(), extra.end());
    }

    return args;
}

std::vector<std::pair<std::string, std::string>>
LlamaCppRouter::build_env_vars(const std::string& llamacpp_backend,
                               const std::string& executable) const {
    std::vector<std::pair<std::string, std::string>> env_vars;

#ifndef _WIN32
    if (is_rocm_backend(llamacpp_backend)) {
        fs::path exe_dir = fs::path(executable).parent_path();
        std::string lib_path = exe_dir.string();

        if (llamacpp_backend == "rocm-stable") {
            std::string runtime_dir =
                BackendUtils::get_install_directory("rocm-stable-runtime", "");
            if (fs::exists(runtime_dir)) {
                lib_path = runtime_dir + ":" + lib_path;
            }
        } else if (llamacpp_backend == "rocm-preview") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_lib =
                    BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_lib.empty()) {
                    lib_path = therock_lib + ":" + lib_path;
                }
            }
        }

        const char* existing = std::getenv("LD_LIBRARY_PATH");
        if (existing && std::strlen(existing) > 0) {
            lib_path = lib_path + ":" + std::string(existing);
        }
        env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    }
#else
    if (is_rocm_backend(llamacpp_backend)) {
        std::string new_path;
        if (llamacpp_backend == "rocm-stable") {
            std::string runtime_dir =
                BackendUtils::get_install_directory("rocm-stable-runtime", "");
            if (fs::exists(runtime_dir)) new_path = runtime_dir;
        } else if (llamacpp_backend == "rocm-preview") {
            std::string rocm_arch = SystemInfo::get_rocm_arch();
            if (!rocm_arch.empty()) {
                std::string therock_bin =
                    BackendUtils::get_therock_lib_path(rocm_arch);
                if (!therock_bin.empty()) new_path = therock_bin;
            }
        }
        if (!new_path.empty()) {
            const char* existing = std::getenv("PATH");
            if (existing && std::strlen(existing) > 0) {
                new_path += ";" + std::string(existing);
            }
            env_vars.push_back({"PATH", new_path});
        }
        if (SystemInfo::get_rocm_arch() == "gfx1151") {
            env_vars.push_back({"OCL_SET_SVM_SIZE", "262144"});
        }
    }
#endif

#ifdef __APPLE__
    const char* no_residency = std::getenv("GGML_METAL_NO_RESIDENCY");
    if (no_residency) {
        env_vars.push_back({"GGML_METAL_NO_RESIDENCY", no_residency});
    }
#endif

    return env_vars;
}

void LlamaCppRouter::start() {
    auto* cfg = RuntimeConfig::global();
    if (!cfg || !cfg->router_mode()) {
        throw std::runtime_error(
            "LlamaCppRouter::start called without router_mode enabled");
    }

    LOG(INFO, kLogTag) << "Starting llama-server in router mode" << std::endl;

    const std::string llamacpp_backend = resolve_backend_choice();
    LOG(INFO, kLogTag) << "  backend: " << llamacpp_backend << std::endl;

    // Keep device_type_ in sync with the selected backend so Router reports
    // the right device for routed models.
    device_type_ = (llamacpp_backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;

    // Ensure the backend binary is installed before we try to spawn it.
    if (backend_manager_) {
        backend_manager_->install_backend(LlamaCppServer::SPEC.recipe,
                                          llamacpp_backend);
    }

    port_ = choose_port();
    const std::string executable =
        BackendUtils::get_backend_binary_path(LlamaCppServer::SPEC,
                                              llamacpp_backend);

    auto args = build_args(llamacpp_backend);
    auto env_vars = build_env_vars(llamacpp_backend, executable);

    if (is_debug()) {
        std::string joined;
        for (const auto& a : args) {
            if (!joined.empty()) joined += ' ';
            joined += a;
        }
        LOG(DEBUG, kLogTag) << "  argv: " << executable << " " << joined
                            << std::endl;
    }

    bool inherit_output = (log_level_ == "info") || is_debug();
    process_handle_ = utils::ProcessManager::start_process(
        executable, args, "", inherit_output, /*filter_health_logs=*/true,
        env_vars);

    if (!wait_for_ready("/health")) {
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
        throw std::runtime_error("llama-server (router) failed to start");
    }

    // Seed a sentinel model_name_ so the Router's debug logging doesn't print
    // an empty string. The sentinel is *not* added to the roster — it only
    // exists so WrappedServer defaults keep working.
    model_name_ = "<llamacpp-router>";
    checkpoint_ = "";
    recipe_options_ = RecipeOptions(kRouterSentinelRecipe, json::object());

    refresh_roster();

    LOG(INFO, kLogTag) << "Router ready on port " << port_
                       << " with " << get_owned_models().size()
                       << " model(s)" << std::endl;
}

void LlamaCppRouter::load(const std::string& /*model_name*/,
                          const ModelInfo& /*model_info*/,
                          const RecipeOptions& /*options*/,
                          bool /*do_not_upgrade*/) {
    // Router mode owns the roster at startup via start(); per-model /load
    // requests that target the router are coalesced to a roster lookup by
    // Router::load_model and never reach this method in practice. If someone
    // does call load() (e.g. install_router_server redirected to load), we
    // lazily start the process.
    if (!is_process_running()) {
        start();
    }
}

void LlamaCppRouter::unload() {
    LOG(INFO, kLogTag) << "Unloading router llama-server" << std::endl;
#ifdef _WIN32
    if (process_handle_.handle) {
#else
    if (process_handle_.pid > 0) {
#endif
        utils::ProcessManager::stop_process(process_handle_);
        process_handle_ = {nullptr, 0};
        port_ = 0;
    }
    std::lock_guard<std::mutex> lock(roster_mutex_);
    roster_.clear();
}

void LlamaCppRouter::refresh_roster() {
    if (!is_process_running()) {
        LOG(DEBUG, kLogTag) << "refresh_roster: child not running, skipping"
                            << std::endl;
        return;
    }

    const std::string url = get_base_url() + "/v1/models";
    utils::HttpResponse resp;
    try {
        resp = utils::HttpClient::get(url);
    } catch (const std::exception& e) {
        LOG(WARNING, kLogTag) << "refresh_roster: GET /v1/models failed: "
                              << e.what() << std::endl;
        return;
    }

    if (resp.status_code != 200) {
        LOG(WARNING, kLogTag) << "refresh_roster: /v1/models returned status "
                              << resp.status_code << std::endl;
        return;
    }

    std::unordered_set<std::string> new_roster;
    try {
        auto body = json::parse(resp.body);
        if (body.contains("data") && body["data"].is_array()) {
            for (const auto& entry : body["data"]) {
                if (entry.contains("id") && entry["id"].is_string()) {
                    new_roster.insert(entry["id"].get<std::string>());
                }
            }
        }
    } catch (const std::exception& e) {
        LOG(WARNING, kLogTag)
            << "refresh_roster: failed to parse /v1/models body: "
            << e.what() << std::endl;
        return;
    }

    // Also register the canonical Lemonade name for each roster entry so the
    // Router, which resolves requested names via ModelManager, matches
    // regardless of whether the caller supplied the public or canonical form.
    if (model_manager_) {
        std::unordered_set<std::string> augmented = new_roster;
        for (const auto& name : new_roster) {
            try {
                std::string canonical =
                    model_manager_->resolve_model_name(name);
                if (!canonical.empty()) augmented.insert(canonical);
            } catch (...) {
                // Unknown to Lemonade registry — that's fine, keep the preset
                // name as-is.
            }
        }
        new_roster = std::move(augmented);
    }

    {
        std::lock_guard<std::mutex> lock(roster_mutex_);
        roster_ = std::move(new_roster);
    }

    LOG(INFO, kLogTag) << "Roster refreshed: " << get_owned_models().size()
                       << " model(s)" << std::endl;
}

bool LlamaCppRouter::owns_model(const std::string& name) const {
    if (name.empty()) return false;
    std::lock_guard<std::mutex> lock(roster_mutex_);
    if (roster_.count(name)) return true;
    // Fall back to canonical form if the caller passed a public name but we
    // only cached the canonical one (refresh_roster tries to cache both, but
    // this is a belt-and-braces check).
    if (model_manager_) {
        try {
            std::string canonical = model_manager_->resolve_model_name(name);
            if (!canonical.empty() && roster_.count(canonical)) return true;
        } catch (...) {
        }
    }
    return false;
}

std::vector<std::string> LlamaCppRouter::get_owned_models() const {
    std::lock_guard<std::mutex> lock(roster_mutex_);
    std::vector<std::string> out(roster_.begin(), roster_.end());
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Request forwarding — mirror LlamaCppServer body-rewrite semantics so the
// upstream model-name-based routing in llama-server can pick the right model.
// ---------------------------------------------------------------------------
json LlamaCppRouter::chat_completion(const json& request) {
    json modified = request;
    if (modified.contains("max_completion_tokens") &&
        !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return forward_request("/v1/chat/completions", modified);
}

json LlamaCppRouter::completion(const json& request) {
    json modified = request;
    if (modified.contains("max_completion_tokens") &&
        !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }
    return forward_request("/v1/completions", modified);
}

json LlamaCppRouter::embeddings(const json& request) {
    return forward_request("/v1/embeddings", request);
}

json LlamaCppRouter::reranking(const json& request) {
    return forward_request("/v1/rerank", request);
}

json LlamaCppRouter::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

} // namespace backends
} // namespace lemon
