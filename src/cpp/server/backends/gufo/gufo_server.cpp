#include "lemon/backends/gufo/gufo_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/gufo/gufo.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/gguf_shard_utils.h"
#include "lemon/model_manager.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"

#include <lemon/utils/aixlog.hpp>

#include <map>
#include <stdexcept>
#include <vector>

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";

// Each speculative mode takes its draft through its own flag.
const std::map<std::string, std::string>& draft_flags() {
    static const std::map<std::string, std::string> kFlags = {
        {"dflash2", "--dflash-model"},
        {"mtp", "--mtp-model"},
        {"dspark", "--dspark-model"},
    };
    return kFlags;
}

class GufoOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        return llamacpp::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        return llamacpp::ops()->find_imported_checkpoint(import_dir);
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        return llamacpp::ops()->validate_registration_checkpoint(checkpoint);
    }

    std::string validate_checkpoint_file(const std::string& resolved_path) const override {
        return llamacpp::ops()->validate_checkpoint_file(resolved_path);
    }
};

}  // namespace

namespace gufo {

std::vector<std::string> build_server_argv(const std::string& model_name,
                                           const std::string& gguf_path,
                                           const std::string& mmproj_path,
                                           const std::string& draft_path,
                                           const std::string& speculative,
                                           int ctx_size,
                                           int port,
                                           const std::string& gufo_args) {
    // gufo answers 404 to a request naming any other model, and Lemonade
    // forwards its own model name.
    std::vector<std::string> argv = {
        "serve", "llm",
        "--model", gguf_path,
        "--served-model-name", model_name,
        "--context", std::to_string(ctx_size),
        "--host", "0.0.0.0",
        "--port", std::to_string(port),
    };
    if (!mmproj_path.empty()) {
        argv.insert(argv.end(), {"--mmproj", mmproj_path});
    }
    if (!draft_path.empty()) {
        const auto flag = draft_flags().find(speculative);
        if (flag == draft_flags().end()) {
            throw std::invalid_argument("gufo: a draft model needs gufo_speculative set to "
                                        "dflash2, mtp or dspark, not '" + speculative + "'");
        }
        argv.insert(argv.end(), {"--speculative", speculative, flag->second, draft_path});
    }

    if (!gufo_args.empty()) {
        const std::string validation_error =
            utils::validate_custom_args(gufo_args, reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom gufo arguments:\n" + validation_error);
        }
        const std::vector<std::string> custom_args = utils::parse_custom_args(gufo_args);
        argv.insert(argv.end(), custom_args.begin(), custom_args.end());
    }
    return argv;
}

}  // namespace gufo

GufoServer::GufoServer(const std::string& log_level, ModelManager* model_manager,
                       BackendManager* backend_manager)
    : WrappedServer("gufo", log_level, model_manager, backend_manager) {}

GufoServer::~GufoServer() {
    unload();
}

void GufoServer::load(const std::string& model_name, const ModelInfo& model_info,
                      const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned image is present
    LOG(INFO, "Gufo") << "Loading model: " << model_name << std::endl;

    backend_manager_->install_backend(gufo::descriptor.recipe, kBackend);

    const std::string gguf_path = model_info.resolved_path();
    if (gguf_path.empty()) {
        throw std::runtime_error("GGUF file not found for checkpoint: " + model_info.checkpoint());
    }
    const std::string mmproj_path = model_info.resolved_path("mmproj");
    const std::string draft_path = model_info.resolved_path("draft");

    const int ctx_size = options.get_option("ctx_size");
    const std::string gufo_args = options.get_option("gufo_args");
    device_type_ = DEVICE_GPU;
    port_ = choose_port();

    ServerCommand command;
    command.program = gufo::descriptor.binary;
    command.args = gufo::build_server_argv(model_name, gguf_path, mmproj_path, draft_path,
                                           model_info.extra<std::string>("gufo_speculative", ""),
                                           ctx_size, port_, gufo_args);
    for (const std::string& path : {gguf_path, mmproj_path, draft_path}) {
        if (!path.empty()) {
            const auto files = gguf_files(path);
            command.model_files.insert(command.model_files.end(), files.begin(), files.end());
        }
    }
    command.port = port_;
    // /health answers as soon as the process is up; /ready waits for the model.
    command.ready_endpoint = "/ready";

    const bool inherit_output = (log_level_ == "info") || is_debug();
    start_server(std::make_unique<ContainerProcess>(
                     ProcessOutput{inherit_output, true}, gufo::descriptor.recipe, kBackend,
                     model_name, *gufo::descriptor.container_for(kBackend),
                     BackendUtils::get_backend_image(gufo::descriptor.recipe, kBackend)),
                 command, utils::HttpClient::get_default_timeout());
    LOG(DEBUG, "Gufo") << "Model loaded on port " << get_backend_port() << std::endl;
}

void GufoServer::unload() {
    stop_server();
}

json GufoServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json GufoServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json GufoServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

namespace gufo {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<GufoServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<GufoOps>();
}

}  // namespace gufo
}  // namespace backends
}  // namespace lemon
