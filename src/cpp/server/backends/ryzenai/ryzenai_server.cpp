#include "backend_definition_resources.h"
#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/ryzenai/ryzenai.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/model_manager.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

InstallParams RyzenAIServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    return {"lemonade-sdk/ryzenai-server", "ryzenai-server.zip"};
}

RyzenAIServer::RyzenAIServer(bool debug, ModelManager* model_manager, BackendManager* backend_manager)
    : RyzenAIServer(debug, model_manager, backend_manager,
                    backends::production_backend_effects(backend_manager, [this] { return choose_port(); })) {}

RyzenAIServer::RyzenAIServer(bool debug, ModelManager* model_manager, BackendManager* backend_manager,
                           backends::BackendEffects effects)
    : WrappedServer("RyzenAI-Server", debug ? "debug" : "info", model_manager, backend_manager), effects_(std::move(effects)) {
    bind_legacy_operations(LegacyOperationProfile::RyzenAI, *this);
}

void RyzenAIServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    LOG(DEBUG, "RyzenAI") << "Loading model: " << model_name << std::endl;
    int ctx_size = options.get_option("ctx_size");

    effects_.binary.install("ryzenai-llm", "npu");

    std::string ryzenai_server_path = effects_.binary.managed_path(*backends::ryzenai::legacy_spec(), "npu");
    if (ryzenai_server_path.empty()) {
        throw std::runtime_error("RyzenAI-Server executable not found even after installation attempt");
    }

    LOG(DEBUG, "RyzenAI") << "Found ryzenai-server at: " << ryzenai_server_path << std::endl;

    const std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !effects_.exists(model_path)) {
        throw std::runtime_error("Model path does not exist: " + model_path);
    }

    LOG(DEBUG, "RyzenAI") << "Model path: " << model_path << std::endl;

    port_ = effects_.allocate_port();

    std::vector<std::string> args = {
        "-m", model_path,
        "--port", std::to_string(port_),
        "--ctx-size", std::to_string(ctx_size)
    };

    if (is_debug()) {
        args.push_back("--verbose");
    }

    LOG(DEBUG, "RyzenAI") << "Starting: \"" << ryzenai_server_path << "\"";
    for (const auto& arg : args) {
        LOG(DEBUG, "RyzenAI") << " \"" << arg << "\"";
    }
    LOG(DEBUG, "RyzenAI") << std::endl;

    ServerCommand command;
    command.program = ryzenai_server_path;
    command.args = std::move(args);
    command.port = port_;
    start_server(effects_.make_process(ProcessOutput{is_debug(), true}, ""), command);
}

json RyzenAIServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json RyzenAIServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json RyzenAIServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

} // namespace lemon

namespace lemon {
namespace backends {
namespace ryzenai {

namespace {
bool configured_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LEMONADE_RYZENAI_CONFIGURED");
        return value && std::string(value) == "1";
    }();
    return enabled;
}
std::shared_ptr<const ConfiguredBackendEntry> configured_entry() {
    static const auto entry = [] {
        auto result = BackendDefinitionStore().validate({"embedded:ryzenai.json", k_ryzenai_definition, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        return ConfiguredBackendEntry::make(result.definition);
    }();
    return entry;
}
}
std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    if (configured_enabled()) return configured_entry()->factory(ctx);
    return std::make_unique<::lemon::RyzenAIServer>(ctx.log_level == "debug", ctx.model_manager,
                                                   ctx.backend_manager);
}


namespace {
class RyzenAiOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&,
                                        const CheckpointResolveContext& ctx) const override {
        // RyzenAI models are a directory containing genai_config.json.
        std::string found = find_imported_checkpoint(ctx.model_cache_path);
        return found.empty() ? ctx.model_cache_path : found;  // dir if not found
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        // The primary artifact is the directory holding genai_config.json.
        std::filesystem::path dir = lemon::utils::path_from_utf8(import_dir);
        if (hf_cache::exists(dir)) {
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(dir, hf_cache::dir_options())) {
                if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                    return lemon::utils::path_to_utf8(entry.path().parent_path());
                }
            }
        }
        return "";  // register the directory itself
    }
};
}  // namespace

const BackendSpec* legacy_spec() {
    static const BackendSpec kSpec("ryzenai-server", descriptor.binary,
                                   ::lemon::RyzenAIServer::get_install_params, /*split=*/false);
    return &kSpec;
}
const BackendOps* legacy_ops() { return single_ops<RyzenAiOps>(); }
const BackendSpec* spec() { return configured_enabled() ? &configured_entry()->spec : legacy_spec(); }
const BackendOps* ops() { return configured_enabled() ? configured_entry()->ops.get() : legacy_ops(); }
}  // namespace ryzenai
}  // namespace backends
}  // namespace lemon
