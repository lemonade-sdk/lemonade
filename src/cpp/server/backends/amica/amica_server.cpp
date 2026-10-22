#include "lemon/backends/amica/amica_server.h"
#include "lemon/backends/amica/amica.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
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
    "containing amica-driver.exe next to the amica\\ directory with amica.dll.";

const char* kEngineNotBuilt =
    "amica-engine was not built with this Lemonade. Configure Lemonade with "
    "-DLEMON_AMICA_ROOT=<AMICA install> and rebuild.";

// Lemonade supplies these to amica-engine itself.
const std::set<std::string>& reserved_engine_flags() {
    static const std::set<std::string> kReserved = {"--model-dir", "--amica-dir", "--model-name",
                                                    "--port", "--host"};
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

    const std::string amica_dir = path_to_utf8(path_from_utf8(tools_dir).parent_path() / "amica");
    if (!fs::is_regular_file(path_from_utf8(amica_dir) / "amica.dll")) {
        throw std::runtime_error("amica.dll was not found in " + amica_dir + ". " + kToolsNotConfigured);
    }

    port_ = choose_port();
    ServerCommand command;
    command.program = engine;
    command.args = amica::build_engine_argv(model_dir, amica_dir, model_name, port_,
                                            options.get_option("amica_args"));
    command.port = port_;

    LOG(INFO, "AMICA") << "Using AMICA: " << amica_dir << std::endl;

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

std::vector<std::string> build_engine_argv(const std::string& model_dir,
                                           const std::string& amica_dir,
                                           const std::string& model_name,
                                           int port,
                                           const std::string& amica_args) {
    std::vector<std::string> argv = {
        "--model-dir", model_dir,
        "--amica-dir", amica_dir,
        "--model-name", model_name,
        "--port", std::to_string(port),
    };

    if (!amica_args.empty()) {
        const std::string error = validate_custom_args(amica_args, reserved_engine_flags());
        if (!error.empty()) {
            throw std::invalid_argument("Invalid custom amica-engine arguments:\n" + error);
        }
        for (const auto& arg : parse_custom_args(amica_args)) {
            argv.push_back(arg);
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
