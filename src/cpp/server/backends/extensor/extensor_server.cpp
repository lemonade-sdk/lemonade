#include "lemon/backends/extensor/extensor_server.h"

#include "lemon/backends/extensor/extensor.h"
#include "lemon/backends/extensor/extensor_install.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

InstallParams ExtensorServer::get_install_params(const std::string& backend,
                                                 const std::string& version) {
    return extensor::install_params(backend, version, get_current_os(), SystemInfo::get_rocm_arch());
}

ExtensorServer::ExtensorServer(const std::string& log_level,
                               ModelManager* model_manager,
                               BackendManager* backend_manager)
    : WrappedServer("extensor-server", log_level, model_manager, backend_manager) {}

void ExtensorServer::load(const std::string& model_name,
                          const ModelInfo& model_info,
                          const RecipeOptions& options,
                          bool do_not_upgrade) {
    (void)do_not_upgrade;

    std::string backend = options.get_option("extensor_backend");
    RuntimeConfig::validate_backend_choice("extensor", backend);

    std::string model_path = model_info.resolved_path();
    if (model_path.empty()) {
        throw std::runtime_error(
            "No EXTENSOR image found for model '" + model_name +
            "'; download the model or register a local image with source 'local_path'");
    }

    std::error_code ec;
    fs::path model = utils::path_from_utf8(model_path);
    if (!fs::is_regular_file(model, ec) || ec) {
        throw std::runtime_error("EXTENSOR model does not exist: " + model_path);
    }
    // Paths must survive NativeProcess changing the child's working directory.
    model_path = utils::path_to_utf8(fs::absolute(model));

    std::string preset = options.get_option("extensor_preset");
    const auto& presets = extensor::preset_option.choices;
    if (std::find(presets.begin(), presets.end(), preset) == presets.end()) {
        throw std::runtime_error(
            "Invalid EXTENSOR preset '" + preset + "'; expected one of " + json(presets).dump());
    }

    backend_manager_->install_backend(extensor::spec()->recipe, backend);
    const fs::path executable_path = fs::absolute(utils::path_from_utf8(
        BackendUtils::get_backend_binary_path(*extensor::spec(), backend)));
    const std::string executable = utils::path_to_utf8(executable_path);

    port_ = choose_port();
    std::vector<std::string> args = {
        "--model", model_path,
        "--preset", preset,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };
    const int ctx_size = options.get_option("ctx_size");
    if (ctx_size > 0) {
        args.push_back("--context-size");
        args.push_back(std::to_string(ctx_size));
    }

    fs::path working_dir = executable_path.parent_path();
    if (working_dir.filename() == "bin" &&
        fs::exists(working_dir.parent_path() / "share" / "extensor", ec) && !ec) {
        working_dir = working_dir.parent_path();
    }

    LOG(INFO, "EXTENSOR") << "Starting extensor-server for " << model_name
                           << " on port " << get_backend_port() << std::endl;

    ServerCommand command;
    command.program = executable;
    command.args = std::move(args);
    command.port = port_;
    command.ready_endpoint = "/readyz";

    const bool inherit_output = (log_level_ == "info") || is_debug();
    try {
        start_server(std::make_unique<NativeProcess>(
                         ProcessOutput{inherit_output, true}, utils::path_to_utf8(working_dir)),
                     command, utils::HttpClient::get_default_timeout());
        const auto response = utils::HttpClient::get(
            get_base_url() + "/v1/models", {}, 10,
            utils::HttpSecurityPolicy::TrustedLoopback);
        if (response.status_code != 200) {
            throw std::runtime_error("Could not query EXTENSOR's loaded model ID");
        }
        const auto models = json::parse(response.body).at("data");
        if (!models.is_array() || models.size() != 1 ||
            !models[0].contains("id") || !models[0]["id"].is_string() ||
            models[0]["id"].get<std::string>().empty()) {
            throw std::runtime_error("EXTENSOR did not report a single loaded model ID");
        }
        backend_model_id_ = models[0]["id"].get<std::string>();
    } catch (...) {
        unload();
        throw;
    }

    LOG(INFO, "EXTENSOR") << "Model loaded on port " << get_backend_port() << std::endl;
}

json ExtensorServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", prepare_request(request));
}

json ExtensorServer::prepare_request(const json& request) const {
    json forwarded = request;
    forwarded["model"] = backend_model_id_;
    return forwarded;
}

void ExtensorServer::forward_streaming_request(const std::string& endpoint,
                                               const std::string& request_body,
                                               httplib::DataSink& sink,
                                               bool sse,
                                               long timeout_seconds,
                                               TelemetryCallback telemetry_callback) {
    WrappedServer::forward_streaming_request(
        endpoint, prepare_request(json::parse(request_body)).dump(), sink,
        sse, timeout_seconds, telemetry_callback);
}

json ExtensorServer::completion(const json& request) {
    (void)request;
    return unsupported_capability_error("text completion");
}

json ExtensorServer::responses(const json& request) {
    (void)request;
    return unsupported_capability_error("responses");
}

namespace extensor {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<ExtensorServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<ExtensorServer>(descriptor);
}

const BackendOps* ops() {
    return default_backend_ops();
}

}  // namespace extensor
}  // namespace backends
}  // namespace lemon
