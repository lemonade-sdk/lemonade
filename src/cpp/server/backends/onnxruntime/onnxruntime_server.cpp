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
#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
// A directory ort-server can actually serve: the graph, the HF tokenizer, and
// an output contract (explicit manifest.json, or config.json to infer from).
bool is_complete_model_dir(const fs::path& dir) {
    return fs::exists(dir / "model.onnx") && fs::exists(dir / "tokenizer.json") &&
           (fs::exists(dir / "manifest.json") || fs::exists(dir / "config.json"));
}

std::vector<fs::path> find_complete_model_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::recursive_directory_iterator(root, hf_cache::dir_options())) {
        if (entry.is_regular_file() && entry.path().filename() == "model.onnx" &&
            is_complete_model_dir(entry.path().parent_path())) {
            dirs.push_back(entry.path().parent_path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}
}  // namespace

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
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    throw std::runtime_error(
        "The onnxruntime backend has no Windows-on-ARM64 build of ort-server yet");
#elif defined(_WIN32)
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
    if (!is_complete_model_dir(path_from_utf8(model_path))) {
        auto candidates = find_complete_model_dirs(path_from_utf8(model_path));
        if (candidates.empty()) {
            throw std::runtime_error(
                "No servable model directory under '" + model_path +
                "': need model.onnx + tokenizer.json + (manifest.json or config.json)");
        }
        std::string listing;
        for (const auto& c : candidates) listing += "\n  " + path_to_utf8(c);
        throw std::runtime_error(
            "Ambiguous model layout under '" + model_path + "': " +
            std::to_string(candidates.size()) +
            " complete model directories found — keep exactly one:" + listing);
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

    bool inherit_output = (log_level_ == "info") || is_debug();
    ProcessHandle started_handle = utils::ProcessManager::start_process(
        executable, args, "", inherit_output, false, {});
    set_process_handle(started_handle);

    if (!has_process_handle(started_handle)) {
        throw std::runtime_error("Failed to start ort-server process");
    }
    LOG(INFO, "OnnxRuntimeServer") << "Process started with PID: " << started_handle.pid << std::endl;

    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("ort-server failed to start or become ready");
    }
    start_backend_watchdog("/health");
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
    return forward_request("/classify", body, 120);
}

json OnnxRuntimeServer::classify(const json& request) {
    // Accept either OpenAI-style "input" or plain "text".
    std::string text;
    if (request.contains("text") && request["text"].is_string()) {
        text = request["text"].get<std::string>();
    } else if (request.contains("input") && request["input"].is_string()) {
        text = request["input"].get<std::string>();
    } else {
        return json{
            {"error", {
                {"message", "Missing 'input' (or 'text') string in classify request"},
                {"type", "invalid_request_error"},
                {"status_code", 400},
            }}
        };
    }
    return forward_classify(text, request);
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

    // Resolve only when the layout is unambiguous: exactly one complete model
    // directory. Anything else returns "" and load() reports a precise error;
    // resolution runs during bulk model listing, so it must never throw.
    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        fs::path dir = path_from_utf8(import_dir);
        if (!hf_cache::exists(dir)) {
            return "";
        }
        auto candidates = find_complete_model_dirs(dir);
        if (candidates.size() != 1) {
            if (candidates.size() > 1) {
                LOG(WARNING, "OnnxRuntimeServer")
                    << candidates.size() << " complete model directories under "
                    << import_dir << "; refusing to pick one" << std::endl;
            }
            return "";
        }
        return path_to_utf8(candidates.front());
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<OnnxRuntimeServer>(descriptor); }
const BackendOps* ops() { return single_ops<OnnxRuntimeOps>(); }

}  // namespace onnxruntime
}  // namespace backends
}  // namespace lemon
