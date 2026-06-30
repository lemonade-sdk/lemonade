#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/backends/ryzenai/ryzenai.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/model_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/utils/path_utils.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/error_types.h"
#include <iostream>
#include <filesystem>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

InstallParams RyzenAIServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    return {"lemonade-sdk/ryzenai-server", "ryzenai-server.zip"};
}

RyzenAIServer::RyzenAIServer(const std::string& model_name, bool debug, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("RyzenAI-Server", debug ? "debug" : "info", model_manager, backend_manager),
      model_name_(model_name),
      is_loaded_(false) {
}

RyzenAIServer::~RyzenAIServer() {
    if (is_loaded_) {
        try {
            unload();
        } catch (...) {
            // Suppress exceptions in destructor
        }
    }
}

bool RyzenAIServer::is_available() {
    try {
        return !backends::BackendUtils::get_backend_binary_path(*backends::ryzenai::spec(), "npu").empty();
    } catch (...) {
        return false;
    }
}

void RyzenAIServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    LOG(DEBUG, "RyzenAI") << "Loading model: " << model_name << std::endl;
    int ctx_size = options.get_option("ctx_size");

    backend_manager_->install_backend("ryzenai-llm", "npu");

    std::string ryzenai_server_path = backends::BackendUtils::get_backend_binary_path(*backends::ryzenai::spec(), "npu");
    if (ryzenai_server_path.empty()) {
        throw std::runtime_error("RyzenAI-Server executable not found even after installation attempt");
    }

    LOG(DEBUG, "RyzenAI") << "Found ryzenai-server at: " << ryzenai_server_path << std::endl;

    if (model_path_.empty()) {
        throw std::runtime_error("Model path is required for RyzenAI-Server. Call set_model_path() before load()");
    }

    if (!fs::exists(model_path_)) {
        throw std::runtime_error("Model path does not exist: " + model_path_);
    }

    model_name_ = model_name;

    LOG(DEBUG, "RyzenAI") << "Model path: " << model_path_ << std::endl;

    port_ = choose_port();

    std::vector<std::string> args = {
        "-m", model_path_,
        "--port", std::to_string(port_),
        "--ctx-size", std::to_string(ctx_size)
    };

    if (is_debug()) {
        args.push_back("--verbose");
    }

    LOG(DEBUG, "RyzenAI") << "Starting: \"" << ryzenai_server_path << "\"";
    for (const auto& arg : args) {
        LOG(DEBUG, "RyzenAI") << " \"" << arg << "\"";
    }
    LOG(DEBUG, "RyzenAI") << std::endl;

    // Start the process (filter health check spam)
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        ryzenai_server_path,
        args,
        "",
        is_debug(),
        true
    );
    set_process_handle(started_handle);

    if (!utils::ProcessManager::is_running(started_handle)) {
        throw std::runtime_error("Failed to start ryzenai-server process");
    }

    LOG(DEBUG, "ProcessManager") << "Process started successfully, PID: "
                << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("RyzenAI-Server failed to start (check logs for details)");
    }

    is_loaded_ = true;
    LOG(INFO, "RyzenAI") << "Model loaded on port " << get_backend_port() << std::endl;
}

void RyzenAIServer::unload() {
    stop_backend_watchdog();
    LOG(DEBUG, "RyzenAI") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        utils::ProcessManager::stop_process(handle);
    }

    is_loaded_ = false;
    model_path_.clear();
}

json RyzenAIServer::chat_completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }

    return forward_request("/v1/chat/completions", request);
}

json RyzenAIServer::completion(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }

    return forward_request("/v1/completions", request);
}

json RyzenAIServer::responses(const json& request) {
    if (!is_loaded_) {
        throw ModelNotLoadedException("RyzenAI-Server");
    }

    return forward_request("/v1/responses", request);
}

} // namespace lemon

namespace lemon {
namespace backends {
namespace ryzenai {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    // RyzenAI requires its model path resolved before load() via set_model_path().
    auto server = std::make_unique<::lemon::RyzenAIServer>(
        ctx.model_info->model_name, ctx.log_level == "debug",
        ctx.model_manager, ctx.backend_manager);
    server->set_model_path(ctx.model_info->resolved_path());
    return server;
}


namespace {
class RyzenAiOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        // RyzenAI models are a directory containing genai_config.json.
        std::string found = find_imported_checkpoint(ctx.model_cache_path);
        return found.empty() ? ctx.model_cache_path : found;  // dir if not found
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        // The primary artifact is the directory holding genai_config.json.
        std::filesystem::path dir = lemon::utils::path_from_utf8(import_dir);
        if (hf_cache::exists(dir)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(dir, hf_cache::dir_options())) {
                if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                    return lemon::utils::path_to_utf8(entry.path().parent_path());
                }
            }
        }
        return "";  // register the directory itself
    }
};
}  // namespace

const BackendSpec* spec() {
    static const BackendSpec kSpec("ryzenai-server", descriptor.binary,
                                   ::lemon::RyzenAIServer::get_install_params, /*split=*/false);
    return &kSpec;
}
const BackendOps* ops() { return single_ops<RyzenAiOps>(); }
}  // namespace ryzenai
}  // namespace backends
}  // namespace lemon
