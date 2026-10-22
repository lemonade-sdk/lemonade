#include "lemon/backends/mediapipe/mediapipe_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/mediapipe/mediapipe.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

constexpr long kStartupTimeoutSeconds = 300;
constexpr long kRequestTimeoutSeconds = 120;
constexpr int kPipTimeoutSeconds = 1800;

fs::path install_dir() {
    return path_from_utf8(BackendUtils::get_install_directory(mediapipe::descriptor.recipe, "cpu"));
}

fs::path venv_python(const fs::path& venv) {
#ifdef _WIN32
    return venv / "Scripts" / "python.exe";
#else
    return venv / "bin" / "python";
#endif
}

fs::path python_in(const fs::path& candidate) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
    if (!fs::is_directory(candidate, ec)) return {};
    for (const auto& option : {venv_python(candidate), candidate / "python3", candidate / "python"}) {
        if (fs::is_regular_file(option, ec)) return option;
    }
    return {};
}

// The Python a user configured with mediapipe.cpu_bin, "" when unset. Throws
// when the configured path is unusable.
std::string configured_python() {
    const std::string value = BackendUtils::find_external_backend_binary(mediapipe::descriptor.recipe, "cpu");
    if (value.empty()) return "";
    const fs::path python = python_in(path_from_utf8(value));
    if (python.empty()) {
        throw std::runtime_error("mediapipe.cpu_bin (" + value + ") has no Python interpreter");
    }
    return path_to_utf8(python);
}

std::string read_first_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

// Runs a command and returns its exit code, keeping the last lines of output for
// an error message.
int run(const std::string& exe, const std::vector<std::string>& args, std::string& tail,
        int timeout_seconds) {
    std::vector<std::string> lines;
    int rc = -1;
    try {
        rc = ProcessManager::run_process_with_output(
            exe, args,
            [&lines](const std::string& line) {
                LOG(DEBUG, "MediaPipeServer") << line << std::endl;
                lines.push_back(line);
                if (lines.size() > 8) lines.erase(lines.begin());
                return true;
            },
            "", timeout_seconds);
    } catch (const std::exception& e) {
        lines.push_back(e.what());
    }
    tail.clear();
    for (const auto& line : lines) tail += (tail.empty() ? "" : " | ") + line;
    return rc;
}

// A Python 3 that can create venvs. The mediapipe wheels are py3-none, so any
// supported CPython 3 version works.
std::pair<std::string, std::vector<std::string>> find_base_python(std::string& error) {
    std::vector<std::pair<std::string, std::vector<std::string>>> candidates;
#ifdef _WIN32
    candidates.push_back({"py", {"-3"}});
    candidates.push_back({"python", {}});
#else
    candidates.push_back({"python3", {}});
    candidates.push_back({"python", {}});
#endif
    for (const auto& [name, prefix] : candidates) {
        const std::string exe = find_executable_in_path(name);
        if (exe.empty()) continue;
        std::vector<std::string> probe = prefix;
        probe.insert(probe.end(), {"-c", "import sys, venv, ensurepip; print(sys.version_info >= (3, 9))"});
        std::string out;
        if (run(exe, probe, out, 30) == 0 && out.find("True") != std::string::npos) {
            return {exe, prefix};
        }
    }
    error = "No Python 3.9+ with venv support found on PATH; install Python (on Debian/Ubuntu also "
            "python3-venv), or point mediapipe.cpu_bin at a Python environment that has mediapipe";
    return {};
}

void report_progress(const DownloadProgressCallback& progress, const std::string& what, int percent,
                     bool complete) {
    if (!progress) return;
    DownloadProgress p;
    p.file = what;
    p.file_index = 1;
    p.total_files = 1;
    p.percent = percent;
    p.complete = complete;
    progress(p);
}

std::string find_task_file(const fs::path& root) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        return root.extension() == ".task" ? path_to_utf8(root) : "";
    }
    std::vector<fs::path> found;
    fs::recursive_directory_iterator it(root, hf_cache::dir_options(), ec);
    if (ec) return "";
    for (auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == ".task") found.push_back(it->path());
    }
    return found.size() == 1 ? path_to_utf8(found.front()) : "";
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

// The Python the server runs with: the configured one, else the managed venv's.
// "" with `error` set when neither is available.
std::string find_python(std::string& error) {
    try {
        const std::string configured = configured_python();
        if (!configured.empty()) return configured;
    } catch (const std::exception& e) {
        error = e.what();
        return "";
    }
    std::error_code ec;
    const fs::path python = venv_python(install_dir() / "venv");
    if (fs::is_regular_file(python, ec)) return path_to_utf8(python);
    error = "The MediaPipe Python environment is not installed; run: lemonade backends install mediapipe:cpu";
    return "";
}

}  // namespace

InstallParams MediaPipeServer::get_install_params(const std::string& backend,
                                                  const std::string& version) {
    (void)backend;
    (void)version;
    throw std::runtime_error("mediapipe is installed as a local Python venv, not a release archive");
}

MediaPipeServer::MediaPipeServer(const std::string& log_level, ModelManager* model_manager,
                                 BackendManager* backend_manager)
    : WrappedServer("mediapipe-server", log_level, model_manager, backend_manager) {
}

void MediaPipeServer::load(const std::string& model_name,
                           const ModelInfo& model_info,
                           const RecipeOptions& options,
                           bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "MediaPipeServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "MediaPipeServer") << "Per-model settings: " << options.to_log_string() << std::endl;
    device_type_ = DEVICE_CPU;

    backend_manager_->install_backend(mediapipe::descriptor.recipe, "cpu");

    std::string error;
    const std::string python = find_python(error);
    if (python.empty()) throw std::runtime_error(error);

    std::error_code ec;
    const fs::path script = path_from_utf8(get_resource_path("resources/mediapipe/mediapipe_server.py"));
    if (!fs::is_regular_file(script, ec)) {
        throw std::runtime_error("mediapipe_server.py missing from lemonade resources: " +
                                 path_to_utf8(script));
    }

    const std::string resolved = model_info.resolved_path();
    if (resolved.empty() || !fs::exists(path_from_utf8(resolved), ec)) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }
    const std::string task = find_task_file(path_from_utf8(resolved));
    if (task.empty()) {
        throw std::runtime_error("No single MediaPipe .task model under '" + resolved + "'");
    }

    const fs::path work_dir = path_from_utf8(get_cache_dir()) / "mediapipe";
    fs::create_directories(work_dir, ec);

    port_ = ProcessManager::find_free_port(8001);
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port for mediapipe-server");
    }
    const fs::path status_file = work_dir / ("startup-" + std::to_string(port_) + ".json");
    fs::remove(status_file, ec);

    std::vector<std::string> args = {
        path_to_utf8(script),
        "--model-path", task,
        "--port", std::to_string(port_),
        "--status-file", path_to_utf8(status_file),
    };
    const std::string extra_args = options.get_option("mediapipe_args");
    if (!extra_args.empty()) {
        const std::set<std::string> reserved = {"--model-path", "--port", "--host", "--status-file"};
        const std::string validation_error = validate_custom_args(extra_args, reserved);
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom mediapipe-server arguments:\n" +
                                        validation_error);
        }
        std::vector<std::string> custom = parse_custom_args(extra_args);
        args.insert(args.end(), custom.begin(), custom.end());
    }

    LOG(INFO, "MediaPipeServer") << "Using Python: " << python << std::endl;
    LOG(INFO, "MediaPipeServer") << "Using model: " << task << std::endl;

    ServerCommand command;
    command.program = python;
    command.args = args;
    command.port = port_;
    command.env = {{"PYTHONNOUSERSITE", "1"}, {"PYTHONPATH", ""}, {"PYTHONUNBUFFERED", "1"}};
    const bool inherit_output = (log_level_ == "info") || is_debug();
    try {
        start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, false},
                                                     path_to_utf8(work_dir)),
                     command, kStartupTimeoutSeconds);
    } catch (const std::runtime_error& e) {
        const std::string details = read_status_error(status_file);
        fs::remove(status_file, ec);
        if (load_cancelled() || details.empty()) throw;
        throw std::runtime_error(std::string(e.what()) + ": " + details);
    }
    fs::remove(status_file, ec);
    LOG(INFO, "MediaPipeServer") << "Server is ready!" << std::endl;
}

json MediaPipeServer::hands(const json& request) {
    json body = {{"images", request.value("image", json::array())}};
    for (const char* key : {"confidence", "max_hands", "mirrored"}) {
        if (request.contains(key)) body[key] = request[key];
    }
    return forward_request("/hands", body, kRequestTimeoutSeconds);
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace mediapipe {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<MediaPipeServer>(ctx);
}

namespace {
class MediaPipeOps : public BackendOps {
public:
    // A checkpoint variant names the .task bundle in the repo.
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        for (const auto& f : repo_files) {
            if (f == main_variant) return std::vector<std::string>{f};
        }
        return std::nullopt;
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        return find_task_file(path_from_utf8(import_dir));
    }

    InstallCheck check_install(const std::string&, bool) const override {
        std::string error;
        const bool found = !find_python(error).empty();
        return {found, found ? "" : error};
    }

    // Builds <bin>/mediapipe/cpu/venv with the pinned mediapipe wheel; a
    // configured mediapipe.cpu_bin needs nothing built.
    bool install_locally(const std::string& backend, bool force,
                         DownloadProgressCallback progress) const override {
        if (backend != "cpu") return false;
        if (!configured_python().empty()) {
            report_progress(progress, "mediapipe:cpu", 100, true);
            return true;
        }

        const std::string pinned = BackendUtils::get_backend_version(descriptor.recipe, "cpu");
        const fs::path dir = install_dir();
        const fs::path venv = dir / "venv";
        const fs::path version_file = dir / "version.txt";
        std::error_code ec;
        if (!force && fs::is_regular_file(venv_python(venv), ec) && read_first_line(version_file) == pinned) {
            report_progress(progress, "mediapipe:cpu", 100, true);
            return true;
        }

        std::string error;
        const auto [base, prefix] = find_base_python(error);
        if (base.empty()) throw std::runtime_error(error);

        LOG(INFO, "MediaPipeServer") << "Creating Python venv at " << path_to_utf8(venv)
                                     << " with mediapipe==" << pinned << std::endl;
        report_progress(progress, "MediaPipe (creating Python venv)", 5, false);
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        std::string tail;
        std::vector<std::string> venv_args = prefix;
        venv_args.insert(venv_args.end(), {"-m", "venv", path_to_utf8(venv)});
        if (run(base, venv_args, tail, 300) != 0) {
            fs::remove_all(dir, ec);
            throw std::runtime_error("Creating the MediaPipe venv failed: " + tail);
        }

        report_progress(progress, "MediaPipe (installing mediapipe " + pinned + ")", 30, false);
        const std::vector<std::string> pip_args = {
            "-m", "pip", "install", "--disable-pip-version-check", "--no-input",
            "mediapipe==" + pinned};
        if (run(path_to_utf8(venv_python(venv)), pip_args, tail, kPipTimeoutSeconds) != 0) {
            fs::remove_all(dir, ec);
            throw std::runtime_error("Installing mediapipe " + pinned + " failed: " + tail);
        }

        std::ofstream(version_file) << pinned << "\n";
        LOG(INFO, "MediaPipeServer") << "mediapipe " << pinned << " installed" << std::endl;
        report_progress(progress, "mediapipe:cpu", 100, true);
        return true;
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<MediaPipeServer>(descriptor); }
const BackendOps* ops() { return single_ops<MediaPipeOps>(); }

}  // namespace mediapipe
}  // namespace backends
}  // namespace lemon
