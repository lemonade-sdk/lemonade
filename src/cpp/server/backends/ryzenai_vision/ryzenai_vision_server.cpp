#include "lemon/backends/ryzenai_vision/ryzenai_vision_server.h"

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/ryzenai_vision/ryzenai_vision.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

// First NPU compile of a detector runs the VAIML compiler and can take well over
// ten minutes; later loads hit the compile cache and are ready in seconds.
constexpr long kFirstCompileTimeoutSeconds = 3600;
constexpr long kDetectTimeoutSeconds = 120;

const char* const kSetupHint =
    "point lemonade at the Ryzen AI Software Python environment: "
    "lemonade config set ryzenaivision.npu_bin=<env dir or python>";

fs::path python_in(const fs::path& candidate) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) {
        return candidate;
    }
    if (!fs::is_directory(candidate, ec)) {
        return {};
    }
#ifdef _WIN32
    const fs::path options[] = {candidate / "python.exe", candidate / "Scripts" / "python.exe"};
#else
    const fs::path options[] = {candidate / "bin" / "python3", candidate / "bin" / "python",
                                candidate / "python3", candidate / "python"};
#endif
    for (const auto& option : options) {
        if (fs::is_regular_file(option, ec)) {
            return option;
        }
    }
    return {};
}

fs::path env_root_of(const fs::path& python) {
    fs::path dir = python.parent_path();
    const std::string leaf = path_to_utf8(dir.filename());
    if (leaf == "bin" || leaf == "Scripts") {
        return dir.parent_path();
    }
    return dir;
}

std::vector<fs::path> list_dir(const fs::path& dir) {
    std::vector<fs::path> entries;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (auto end = fs::directory_iterator(); !ec && it != end; it.increment(ec)) {
        entries.push_back(it->path());
    }
    return entries;
}

// The Ryzen AI Software release installed in an environment, read from the
// VitisAI EP runtime package metadata ("voe-1.8.0.dist-info" -> "1.8.0").
std::string ryzen_ai_version(const fs::path& python) {
    const fs::path root = env_root_of(python);
    std::vector<fs::path> site_dirs = {root / "Lib" / "site-packages"};
    for (const auto& lib : list_dir(root / "lib")) {
        if (path_to_utf8(lib.filename()).rfind("python", 0) == 0) {
            site_dirs.push_back(lib / "site-packages");
        }
    }
    const std::string prefix = "voe-";
    const std::string suffix = ".dist-info";
    for (const auto& site : site_dirs) {
        for (const auto& entry : list_dir(site)) {
            const std::string name = path_to_utf8(entry.filename());
            if (name.size() > prefix.size() + suffix.size() && name.rfind(prefix, 0) == 0 &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            }
        }
    }
    return "";
}

fs::path server_script() {
    return path_from_utf8(get_resource_path("resources/ryzenai_vision/ryzenai_vision_server.py"));
}

// model.onnx wins (the Hugging Face layout), otherwise the only .onnx file;
// "" when absent or ambiguous.
std::string find_onnx_model(const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        return root.extension() == ".onnx" ? path_to_utf8(root) : "";
    }
    std::vector<fs::path> named;
    std::vector<fs::path> any;
    fs::recursive_directory_iterator it(root, hf_cache::dir_options(), ec);
    if (ec) return "";
    for (auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == ".onnx") {
            any.push_back(it->path());
            if (it->path().filename() == "model.onnx") named.push_back(it->path());
        }
    }
    if (named.size() == 1) return path_to_utf8(named.front());
    if (any.size() == 1) return path_to_utf8(any.front());
    return "";
}

std::string read_status_error(const fs::path& status_file) {
    std::ifstream in(status_file);
    if (!in) return "";
    try {
        json status = json::parse(in);
        return status.value("error", "");
    } catch (...) {
        return "";
    }
}

}  // namespace

namespace ryzenai_vision {

std::string find_python(std::string& error) {
    std::string source = "ryzenaivision.npu_bin";
    std::string value;
    try {
        value = BackendUtils::find_external_backend_binary(descriptor.recipe, "npu");
    } catch (const std::exception& e) {
        error = e.what();
        return "";
    }
#ifndef _WIN32
    // On Windows this variable names the install directory, not a Python env.
    if (value.empty()) {
        source = "RYZEN_AI_INSTALLATION_PATH";
        value = get_environment_variable_utf8(source);
    }
#endif
    if (value.empty()) {
        error = std::string("Ryzen AI Software not found; ") + kSetupHint;
        return "";
    }
    const fs::path python = python_in(path_from_utf8(value));
    if (python.empty()) {
        error = source + " (" + value + ") has no Python interpreter; " + kSetupHint;
        return "";
    }
    if (ryzen_ai_version(python).empty()) {
        error = source + " (" + value + ") is not a Ryzen AI Software environment "
                "(no VitisAI EP 'voe' package); " + kSetupHint;
        return "";
    }
    error.clear();
    return path_to_utf8(python);
}

}  // namespace ryzenai_vision

InstallParams RyzenAIVisionServer::get_install_params(const std::string& backend,
                                                      const std::string& version) {
    (void)backend;
    (void)version;
    throw std::runtime_error(
        std::string("ryzenai-vision has nothing to download; ") + kSetupHint);
}

RyzenAIVisionServer::RyzenAIVisionServer(const std::string& log_level,
                                         ModelManager* model_manager,
                                         BackendManager* backend_manager)
    : WrappedServer("ryzenai-vision-server", log_level, model_manager, backend_manager) {
}

void RyzenAIVisionServer::load(const std::string& model_name,
                               const ModelInfo& model_info,
                               const RecipeOptions& options,
                               bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "RyzenAIVisionServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "RyzenAIVisionServer") << "Per-model settings: " << options.to_log_string() << std::endl;
    device_type_ = DEVICE_NPU;

    std::string error;
    const std::string python = ryzenai_vision::find_python(error);
    if (python.empty()) {
        throw std::runtime_error(error);
    }

    const fs::path script = server_script();
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) {
        throw std::runtime_error("ryzenai_vision_server.py missing from lemonade resources: " +
                                 path_to_utf8(script));
    }

    const std::string resolved = model_info.resolved_path();
    if (resolved.empty() || !fs::exists(path_from_utf8(resolved), ec)) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }
    const std::string onnx = find_onnx_model(path_from_utf8(resolved));
    if (onnx.empty()) {
        throw std::runtime_error("No single .onnx detector under '" + resolved +
                                 "' (name it model.onnx or pick it with the checkpoint variant)");
    }

    const fs::path cache_dir = path_from_utf8(get_cache_dir()) / "ryzenai-vision";
    fs::create_directories(cache_dir, ec);

    port_ = ProcessManager::find_free_port(8001);
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port for ryzenai-vision-server");
    }
    const fs::path status_file = cache_dir / ("startup-" + std::to_string(port_) + ".json");
    fs::remove(status_file, ec);

    std::vector<std::string> args = {
        path_to_utf8(script),
        "--model-path", onnx,
        "--port", std::to_string(port_),
        "--cache-dir", path_to_utf8(cache_dir / "npu-cache"),
        "--status-file", path_to_utf8(status_file),
    };

    const std::string extra_args = options.get_option("ryzenaivision_args");
    if (!extra_args.empty()) {
        const std::set<std::string> reserved = {"--model-path", "--port", "--host", "--device",
                                                "--cache-dir", "--status-file"};
        std::string validation_error = validate_custom_args(extra_args, reserved);
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ryzenai-vision-server arguments:\n" +
                                        validation_error);
        }
        std::vector<std::string> custom = parse_custom_args(extra_args);
        args.insert(args.end(), custom.begin(), custom.end());
    }

    LOG(INFO, "RyzenAIVisionServer") << "Using Python: " << python << std::endl;
    LOG(INFO, "RyzenAIVisionServer") << "Using model: " << onnx << std::endl;

    ServerCommand command;
    command.program = python;
    command.args = args;
    command.port = port_;
    // Keep the Ryzen AI environment's own packages authoritative even when
    // lemond was started from a shell with another venv or PYTHONPATH active.
    command.env = {
        {"PYTHONNOUSERSITE", "1"},
        {"PYTHONPATH", ""},
        {"PYTHONUNBUFFERED", "1"},
    };
    const bool inherit_output = (log_level_ == "info") || is_debug();
    try {
        // The VitisAI compiler drops signature files into the working directory.
        start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, false},
                                                     path_to_utf8(cache_dir)),
                     command, kFirstCompileTimeoutSeconds);
    } catch (const std::runtime_error& e) {
        const std::string details = read_status_error(status_file);
        fs::remove(status_file, ec);
        if (load_cancelled() || details.empty()) throw;
        throw std::runtime_error(std::string(e.what()) + ": " + details);
    }
    fs::remove(status_file, ec);
    LOG(INFO, "RyzenAIVisionServer") << "Server is ready!" << std::endl;
}

json RyzenAIVisionServer::detect(const json& request) {
    json body = {{"images", request.value("image", json::array())}};
    for (const char* key : {"confidence", "classes", "max_detections"}) {
        if (request.contains(key)) body[key] = request[key];
    }
    return forward_request("/detect", body, kDetectTimeoutSeconds);
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace ryzenai_vision {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<RyzenAIVisionServer>(ctx);
}

namespace {
class RyzenAIVisionOps : public BackendOps {
public:
    // A checkpoint variant names the .onnx graph in the repo (e.g.
    // "onnx/model.onnx"); fetch only that graph (plus its external-data file, if
    // any) rather than every quantized sibling the repo also ships.
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        std::vector<std::string> files;
        for (const auto& f : repo_files) {
            if (f == main_variant || f == main_variant + "_data" || f == main_variant + ".data") {
                files.push_back(f);
            }
        }
        if (files.empty()) {
            return std::nullopt;
        }
        return files;
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        return find_onnx_model(path_from_utf8(import_dir));
    }

    std::string resolve_version(const std::string&, const std::string& file_version) const override {
        std::string error;
        const std::string python = find_python(error);
        if (python.empty()) return file_version;
        const std::string version = ryzen_ai_version(path_from_utf8(python));
        return version.empty() ? file_version : version;
    }

    InstallCheck check_install(const std::string&, bool) const override {
        std::string error;
        const bool found = !find_python(error).empty();
        return {found, found ? "" : error};
    }

    std::optional<UnavailableState> classify_unavailable(
        const std::string&, const std::string& install_error, const std::string&) const override {
        return UnavailableState{
            "action_required",
            install_error.empty() ? "Ryzen AI Software not found." : install_error,
            "https://ryzenai.docs.amd.com/en/latest/inst.html",
            false,
        };
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<RyzenAIVisionServer>(descriptor); }
const BackendOps* ops() { return single_ops<RyzenAIVisionOps>(); }

}  // namespace ryzenai_vision
}  // namespace backends
}  // namespace lemon
