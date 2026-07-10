#include "lemon/backends/onnxruntime/onnxruntime_server.h"
#include "lemon/backends/onnxruntime/onnxruntime.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backend_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

// The ort-server subprocess speaks a tiny HTTP contract:
//   GET  /health             -> 200 when the model is loaded and ready
//   POST /classify {text}    -> 200 {"labels": {"<label>": <score in [0,1]>, ...}}
// It runs one exported ONNX model (seq- or token-classification) on the CPU EP.
// Distributed as a self-contained bundle by lemonade-sdk/ort-server.
InstallParams OnnxRuntimeServer::get_install_params(const std::string& backend,
                                                    const std::string& version) {
    (void)backend;  // CPU-only for v1
    InstallParams params;
    params.repo = "lemonade-sdk/ort-server";
#ifdef _WIN32
    params.filename = "ort-server-" + version + "-windows-x64.zip";
#elif defined(__APPLE__)
    params.filename = "ort-server-" + version + "-macos-arm64.tar.gz";
#elif defined(__aarch64__) || defined(_M_ARM64)
    params.filename = "ort-server-" + version + "-linux-arm64.tar.gz";
#else
    params.filename = "ort-server-" + version + "-linux-x64.tar.gz";
#endif
    return params;
}

OnnxRuntimeServer::OnnxRuntimeServer(const std::string& log_level, ModelManager* model_manager,
                                     BackendManager* backend_manager)
    : WrappedServer("ort-server", log_level, model_manager, backend_manager) {
}

OnnxRuntimeServer::~OnnxRuntimeServer() {
    unload();
}

void OnnxRuntimeServer::load(const std::string& model_name,
                             const ModelInfo& model_info,
                             const RecipeOptions& options,
                             bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "OnnxRuntimeServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "OnnxRuntimeServer") << "Per-model settings: " << options.to_log_string() << std::endl;

    std::string extra_args = options.get_option("onnxruntime_args");
    device_type_ = DEVICE_CPU;

    backend_manager_->install_backend(onnxruntime::spec()->recipe, "cpu");

    std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !fs::exists(model_path)) {
        throw std::runtime_error("Model directory not found for checkpoint: " + model_info.checkpoint());
    }
    LOG(INFO, "OnnxRuntimeServer") << "Using model: " << model_path << std::endl;

    std::string executable = BackendUtils::get_backend_binary_path(*onnxruntime::spec(), "cpu");
    LOG(INFO, "OnnxRuntimeServer") << "Using executable: " << executable << std::endl;

    port_ = utils::ProcessManager::find_free_port(8001);
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port for ort-server");
    }

    std::vector<std::string> args = {
        "--model-path", model_path,
        "--port", std::to_string(port_),
    };

    std::set<std::string> reserved_flags = {"--model-path", "--port"};
    if (!extra_args.empty()) {
        std::string validation_error = validate_custom_args(extra_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ort-server arguments:\n" + validation_error);
        }
        std::vector<std::string> custom_args_vec = parse_custom_args(extra_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    std::vector<std::pair<std::string, std::string>> env_vars;
    env_vars.push_back({"PYTHONNOUSERSITE", "1"});

    bool inherit_output = (log_level_ == "info") || is_debug();
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        executable, args, "", inherit_output, false, env_vars);
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start ort-server process");
    }
    LOG(INFO, "OnnxRuntimeServer") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("ort-server failed to start or become ready");
    }
    LOG(INFO, "OnnxRuntimeServer") << "Server is ready!" << std::endl;
}

void OnnxRuntimeServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "OnnxRuntimeServer") << "Stopping server (PID: " << handle.pid << ")" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

json OnnxRuntimeServer::forward_classify(const std::string& text, const json& params) {
    json body = {{"text", text}};
    if (params.contains("top_k")) body["top_k"] = params["top_k"];

    const std::string url = "http://127.0.0.1:" + std::to_string(get_backend_port()) + "/classify";
    auto res = utils::HttpClient::post(url, body.dump(), {{"Content-Type", "application/json"}}, 0);

    if (res.status_code != 200) {
        std::string err_msg = res.body;
        try {
            json error_json = json::parse(res.body);
            if (error_json.contains("error")) {
                if (error_json["error"].is_string()) {
                    err_msg = error_json["error"].get<std::string>();
                } else if (error_json["error"].is_object() && error_json["error"].contains("message")) {
                    err_msg = error_json["error"]["message"].get<std::string>();
                }
            }
        } catch (...) {
            // keep raw body
        }
        int status_code = (res.status_code == 400) ? 400 : 500;
        return json{
            {"error", {
                {"message", "Classification failed: " + err_msg},
                {"type", status_code == 400 ? "invalid_request_error" : "classification_error"},
                {"status_code", status_code},
            }}
        };
    }

    try {
        return json::parse(res.body);
    } catch (const json::parse_error& e) {
        return json{
            {"error", {
                {"message", std::string("ort-server returned invalid JSON: ") + e.what()},
                {"type", "classification_error"},
                {"status_code", 500},
            }}
        };
    }
}

json OnnxRuntimeServer::classify(const json& request) {
    try {
        // Accept either OpenAI-style "input" or plain "text".
        std::string text;
        if (request.contains("text") && request["text"].is_string()) {
            text = request["text"].get<std::string>();
        } else if (request.contains("input") && request["input"].is_string()) {
            text = request["input"].get<std::string>();
        } else {
            throw std::runtime_error("Missing 'input' (or 'text') string in classify request");
        }
        return forward_classify(text, request);
    } catch (const std::exception& e) {
        return json{
            {"error", {
                {"message", std::string("Classification failed: ") + e.what()},
                {"type", "invalid_request_error"},
                {"status_code", 400},
            }}
        };
    }
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace onnxruntime {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<OnnxRuntimeServer>(ctx);
}

namespace {
// ort-server models are a directory (model.onnx + tokenizer.json + manifest.json).
// The whole repo downloads by default; resolve to the directory that holds
// model.onnx so the subprocess is launched with --model-path <dir>.
class OnnxRuntimeOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        std::string found = find_imported_checkpoint(ctx.model_cache_path);
        return found.empty() ? ctx.model_cache_path : found;
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        fs::path dir = path_from_utf8(import_dir);
        if (hf_cache::exists(dir)) {
            for (const auto& entry :
                 fs::recursive_directory_iterator(dir, hf_cache::dir_options())) {
                if (entry.is_regular_file() && entry.path().filename() == "model.onnx") {
                    return path_to_utf8(entry.path().parent_path());
                }
            }
        }
        return "";
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<OnnxRuntimeServer>(descriptor); }
const BackendOps* ops() { return single_ops<OnnxRuntimeOps>(); }

}  // namespace onnxruntime
}  // namespace backends
}  // namespace lemon
