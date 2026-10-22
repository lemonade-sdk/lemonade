#include "lemon/backends/amica/amica_server.h"
#include "lemon/backends/amica/amica.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

const char* kToolsNotConfigured =
    "AMICA is not distributed with Lemonade. Point it at an AMICA install with "
    "`lemonade config set amica.dx12_bin=<AMICA install>\\tools`, the directory "
    "containing amica-driver.exe and chatapp\\.";

const char* kEngineNotBuilt =
    "amica-engine was not built with this Lemonade. Configure Lemonade with "
    "-DLEMON_AMICA_ROOT=<AMICA install> and rebuild.";

// amica_server.py supplies the AMICA package folder itself.
const std::set<std::string>& reserved_engine_flags() {
    static const std::set<std::string> kReserved = {"--amica-dir"};
    return kReserved;
}

}  // namespace

InstallParams AmicaServer::get_install_params(const std::string& /*backend*/,
                                              const std::string& /*version*/) {
    throw std::runtime_error(kToolsNotConfigured);
}

AmicaServer::AmicaServer(const std::string& log_level, ModelManager* model_manager,
                         BackendManager* backend_manager)
    : WrappedServer("amica-server", log_level, model_manager, backend_manager) {
}

void AmicaServer::load(const std::string& model_name,
                       const ModelInfo& model_info,
                       const RecipeOptions& options,
                       bool do_not_upgrade) {
    (void)do_not_upgrade;
    LOG(INFO, "AMICA") << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, "AMICA") << "Per-model settings: " << options.to_log_string() << std::endl;

    std::string tools_dir;
    try {
        tools_dir = path_to_utf8(
            path_from_utf8(BackendUtils::get_backend_binary_path(*amica::spec(), "dx12")).parent_path());
    } catch (const std::exception&) {
        throw std::runtime_error(kToolsNotConfigured);
    }

    const std::string engine = amica::engine_path();
    if (!fs::is_regular_file(path_from_utf8(engine))) {
        throw std::runtime_error(kEngineNotBuilt);
    }

    const std::string model_dir = model_info.resolved_path();
    if (model_dir.empty() || !fs::is_directory(path_from_utf8(model_dir))) {
        throw std::runtime_error("AMICA model directory not found for checkpoint: " +
                                 model_info.checkpoint());
    }

    const std::string script = get_resource_path("resources/amica/amica_server.py");
    if (!fs::exists(path_from_utf8(script))) {
        throw std::runtime_error("amica_server.py was not found: " + script);
    }

    port_ = choose_port();
    ServerCommand command;
    command.program = amica::find_python(tools_dir);
    command.args = amica::build_server_argv(script, model_dir, tools_dir, engine, model_name, port_,
                                            options.get_option("amica_args"));
    command.env = {{"PYTHONNOUSERSITE", "1"}, {"PYTHONUNBUFFERED", "1"}};
    command.port = port_;

    LOG(INFO, "AMICA") << "Using AMICA tools: " << tools_dir << std::endl;
    LOG(DEBUG, "AMICA") << "Using Python: " << command.program << std::endl;

    const bool inherit_output = log_level_ == "info" || is_debug();
    start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, true}), command);
}

json AmicaServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json AmicaServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

namespace amica {

std::string engine_path() {
#ifdef _WIN32
    const char* name = "amica-engine.exe";
#else
    const char* name = "amica-engine";
#endif
    return path_to_utf8(path_from_utf8(get_executable_dir()) / name);
}

std::string find_python(const std::string& tools_dir) {
    const std::string override_path = get_environment_variable_utf8("LEMONADE_AMICA_PYTHON");
    if (!override_path.empty()) {
        if (!fs::is_regular_file(path_from_utf8(override_path))) {
            throw std::runtime_error("LEMONADE_AMICA_PYTHON does not name a file: " +
                                     override_path);
        }
        return override_path;
    }

    // Matches the machine tag Chatapp.ps1 gives the environment it bootstraps;
    // the venv records an absolute interpreter path, so it is never shared.
    std::string machine = get_environment_variable_utf8("COMPUTERNAME");
    std::transform(machine.begin(), machine.end(), machine.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (machine.empty()) {
        machine = "local";
    }

    fs::path venv = path_from_utf8(tools_dir) / "chatapp" / ".venv" / ("frontend-" + machine);
#ifdef _WIN32
    fs::path python = venv / "Scripts" / "python.exe";
#else
    fs::path python = venv / "bin" / "python";
#endif
    if (!fs::is_regular_file(python)) {
        throw std::runtime_error(
            "The AMICA ChatApp Python environment was not found at " + path_to_utf8(python) +
            ". Run Chatapp.ps1 from the AMICA tools directory once to create it, or set "
            "LEMONADE_AMICA_PYTHON to a Python with the ChatApp frontend requirements.");
    }
    return path_to_utf8(python);
}

std::vector<std::string> build_server_argv(const std::string& script_path,
                                           const std::string& model_dir,
                                           const std::string& tools_dir,
                                           const std::string& engine,
                                           const std::string& model_name,
                                           int port,
                                           const std::string& amica_args) {
    std::vector<std::string> argv = {
        script_path,
        "--model-dir", model_dir,
        "--tools-dir", tools_dir,
        "--engine", engine,
        "--model-name", model_name,
        "--port", std::to_string(port),
    };

    if (!amica_args.empty()) {
        const std::string error = validate_custom_args(amica_args, reserved_engine_flags());
        if (!error.empty()) {
            throw std::invalid_argument("Invalid custom amica-engine arguments:\n" + error);
        }
        // The --engine-arg=VALUE form keeps argparse from reading a dashed
        // engine flag as one of amica_server.py's own options.
        for (const auto& arg : parse_custom_args(amica_args)) {
            argv.push_back("--engine-arg=" + arg);
        }
    }
    return argv;
}

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AmicaServer>(ctx);
}

namespace {
class AmicaOps : public BackendOps {
public:
    void download_model(const ModelInfo& info, bool /*do_not_upgrade*/,
                        DownloadProgressCallback /*progress*/,
                        const BackendOpsContext& /*ctx*/) const override {
        throw std::runtime_error(
            "AMICA models are local ChatApp bundles and cannot be downloaded. Register '" +
            info.model_name + "' in user_models.json with \"source\": \"local_path\" and its "
            "bundle directory as the checkpoint.");
    }

    InstallCheck check_install(const std::string& /*backend*/, bool binary_found) const override {
        if (!binary_found) {
            return {false, kToolsNotConfigured};
        }
        if (!fs::is_regular_file(path_from_utf8(engine_path()))) {
            return {false, kEngineNotBuilt};
        }
        return {true, ""};
    }

    std::optional<UnavailableState> classify_unavailable(
        const std::string& /*backend*/, const std::string& install_error,
        const std::string& /*default_install_command*/) const override {
        if (install_error == kEngineNotBuilt) {
            return UnavailableState{"action_required", kEngineNotBuilt,
                                    "cmake -DLEMON_AMICA_ROOT=<AMICA install> <build dir>"};
        }
        return UnavailableState{"action_required", kToolsNotConfigured,
                                "lemonade config set amica.dx12_bin=<AMICA install>\\tools"};
    }
};
}  // namespace

const BackendSpec* spec() { return make_spec<AmicaServer>(descriptor); }
const BackendOps* ops() { return single_ops<AmicaOps>(); }

}  // namespace amica
}  // namespace backends
}  // namespace lemon
