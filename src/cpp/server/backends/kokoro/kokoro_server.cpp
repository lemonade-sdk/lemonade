#include "lemon/backends/kokoro/kokoro_server.h"
#include "lemon/backends/kokoro/kokoro.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include <filesystem>
#include "lemon/backend_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
#include <vector>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
// Kokoro doesn't expose backend selection through RuntimeConfig; we resolve it
// from the host platform so each OS gets the right binary archive.
std::string default_kokoro_backend() {
#if defined(__APPLE__)
    return "metal";
#else
    return "cpu";
#endif
}
}

InstallParams KokoroServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/Kokoros";

    if (backend == "cpu") {
#ifdef _WIN32
        params.filename = "kokoros-windows-x86_64.tar.gz";
#elif defined(__linux__)
        params.filename = "kokoros-linux-x86_64.tar.gz";
#endif
    } else if (backend == "metal") {
#if defined(__APPLE__)
        params.filename = "kokoros-darwin-arm64-metal.tar.gz";
#endif
    } else {
        throw std::runtime_error("[KokoroServer] Unknown kokoros backend: " + backend);
    }

    return params;
}

KokoroServer::KokoroServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("kokoro-server", log_level, model_manager, backend_manager) {

}

KokoroServer::~KokoroServer() {
    unload();
}

void KokoroServer::load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade) {
    LOG(INFO, "KokoroServer") << "Loading model: " << model_name << std::endl;

    const std::string backend = default_kokoro_backend();
    backend_manager_->install_backend(kokoro::spec()->recipe, backend);

    fs::path model_path = fs::path(model_info.resolved_path());
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }

    json model_index;

    try {
        LOG(INFO, "KokoroServer") << "Reading " << model_path.filename() << std::endl;
        model_index = JsonUtils::load_from_file(model_path.string());
    } catch (const std::exception& e) {
        throw std::runtime_error("Warning: Could not load " + model_path.filename().string() + ": " + e.what());
    }

    LOG(INFO, "KokoroServer") << "Using model: " << model_index["model"] << std::endl;

    std::string exe_path = BackendUtils::get_backend_binary_path(*kokoro::spec(), backend);

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "KokoroServer") << "Starting server on port " << port_ << std::endl;

    std::vector<std::pair<std::string, std::string>> env_vars;
    fs::path exe_dir = fs::path(exe_path).parent_path();
    env_vars.push_back({"ESPEAK_DATA_PATH", (exe_dir / "espeak-ng-data").string()});
#ifndef _WIN32
    std::string lib_path = exe_dir.string();
    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && strlen(existing_ld_path) > 0) {
        lib_path = lib_path + ":" + std::string(existing_ld_path);
    }

    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
    LOG(INFO, "KokoroServer") << "Setting LD_LIBRARY_PATH=" << lib_path << std::endl;
#endif

    // Note: Don't include exe_path here - ProcessManager::start_process already handles it
    fs::path model_dir = model_path.parent_path();
    std::vector<std::string> args = {
        "-m", (model_dir / model_index["model"]).string(),
        "-d", (model_dir / model_index["voices"]).string(),
        "openai",
        "--ip", "127.0.0.1",
        "--port", std::to_string(port_)
    };

    ProcessHandle started_handle = utils::ProcessManager::start_process(
        exe_path,
        args,
        "",     // working_dir (empty = current)
        is_debug(),  // inherit_output
        false,
        env_vars
    );
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start koko process");
    }

    LOG(INFO, "KokoroServer") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/")) {
        unload();
        throw std::runtime_error("koko failed to start or become ready");
    }
}

void KokoroServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "KokoroServer") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

// ICompletionServer implementation (not supported - return errors)
json KokoroServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

void KokoroServer::audio_speech(const json& request, httplib::DataSink& sink) {
    json tts_request = request;
    tts_request["model"] = "kokoro";

    // OpenAI does not define "stream" for the speech endpoint
    // relying solely on stream_format. Kokoros requires this boolean
    if (request.contains("stream_format")) {
        tts_request["stream"] = true;
    }

    forward_streaming_request("/v1/audio/speech", tts_request.dump(), sink, false);
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace kokoro {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<KokoroServer>(ctx);
}


namespace {
class KokoroOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        // Kokoro models are a directory; resolve to the index.json file inside.
        std::filesystem::path dir = lemon::utils::path_from_utf8(ctx.model_cache_path);
        if (hf_cache::exists(dir)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(dir, hf_cache::dir_options())) {
                if (entry.is_regular_file() && entry.path().filename() == "index.json") {
                    return lemon::utils::path_to_utf8(entry.path());
                }
            }
        }
        return ctx.model_cache_path;  // directory even if index not found
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<KokoroServer>(descriptor); }
const BackendOps* ops() { return single_ops<KokoroOps>(); }
}  // namespace kokoro
}  // namespace backends
}  // namespace lemon
