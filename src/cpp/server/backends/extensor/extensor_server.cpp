#include "lemon/backends/extensor/extensor_server.h"

#include "lemon/backends/extensor/extensor.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

InstallParams ExtensorServer::get_install_params(const std::string& backend,
                                                 const std::string& version) {
    (void)backend;
    (void)version;
    throw std::runtime_error(
        "Automatic EXTENSOR installation is not available; configure extensor.rocm_bin");
}

ExtensorServer::ExtensorServer(const std::string& log_level,
                               ModelManager* model_manager,
                               BackendManager* backend_manager)
    : WrappedServer("extensor-server", log_level, model_manager, backend_manager) {}

ExtensorServer::~ExtensorServer() {
    unload();
}

void ExtensorServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool do_not_upgrade) {
    (void)do_not_upgrade;

    std::string backend = options.get_option("extensor_backend");
    RuntimeConfig::validate_backend_choice("extensor", backend);

    std::string executable = BackendUtils::get_backend_binary_path(*extensor::spec(), backend);
    std::string model_path = options.get_option("extensor_model_path");
    if (model_path.empty()) {
        model_path = model_info.resolved_path();
    }

    if (model_path.empty()) {
        throw std::runtime_error(
            "EXTENSOR model path is not configured; set extensor.extensor_model_path");
    }

    std::error_code ec;
    fs::path model = utils::path_from_utf8(model_path);
    if (!fs::is_regular_file(model, ec) || ec) {
        throw std::runtime_error("EXTENSOR model does not exist: " + model_path);
    }

    std::string preset = options.get_option("extensor_preset");
    if (preset != "exact" && preset != "demo" && preset != "fast") {
        throw std::runtime_error(
            "Invalid EXTENSOR preset '" + preset + "'; expected exact, demo, or fast");
    }

    port_ = choose_port();
    std::vector<std::string> args = {
        "--model", model_path,
        "--preset", preset,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    fs::path executable_path = utils::path_from_utf8(executable);
    fs::path working_dir = executable_path.parent_path();
    if (working_dir.filename() == "bin" &&
        fs::exists(working_dir.parent_path() / "share" / "extensor", ec) && !ec) {
        working_dir = working_dir.parent_path();
    }

    LOG(INFO, "EXTENSOR") << "Starting extensor-server for " << model_name
                           << " on port " << get_backend_port() << std::endl;

    bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(utils::ProcessManager::start_process(
        executable, args, utils::path_to_utf8(working_dir), inherit_output, true));

    if (!wait_for_ready("/readyz", utils::HttpClient::get_default_timeout())) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("extensor-server failed to start");
    }

    LOG(INFO, "EXTENSOR") << "Model loaded on port " << get_backend_port() << std::endl;
}

void ExtensorServer::unload() {
    stop_backend_watchdog();
    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        LOG(INFO, "EXTENSOR") << "Stopping extensor-server" << std::endl;
        utils::ProcessManager::stop_process(handle);
    }
}

json ExtensorServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json ExtensorServer::completion(const json& request) {
    (void)request;
    return unsupported_capability_error("text completion");
}

json ExtensorServer::responses(const json& request) {
    (void)request;
    return unsupported_capability_error("responses");
}

namespace {
class ExtensorOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        if (auto* config = RuntimeConfig::global()) {
            RecipeOptions options("extensor", config->recipe_options(""));
            std::string configured = options.get_option("extensor_model_path");
            if (!configured.empty()) {
                return configured;
            }
        }
        return BackendOps::resolve_checkpoint_path(info, ctx);
    }

    bool is_downloaded(const ModelInfo& info, const BackendOpsContext&) const override {
        std::error_code ec;
        const std::string path = info.resolved_path();
        return !path.empty() && fs::is_regular_file(utils::path_from_utf8(path), ec) && !ec;
    }

    void download_model(const ModelInfo&, bool, DownloadProgressCallback,
                        const BackendOpsContext&) const override {
        throw std::runtime_error(
            "Automatic EXTENSOR model download is not available; "
            "configure extensor.extensor_model_path");
    }
};
}  // namespace

namespace extensor {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<ExtensorServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<ExtensorServer>(descriptor);
}

const BackendOps* ops() {
    return single_ops<ExtensorOps>();
}

}  // namespace extensor
}  // namespace backends
}  // namespace lemon
