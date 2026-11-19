#include "lemon/backends/halogen/halogen_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/halogen/halogen.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/container_manager.h"
#include "lemon/utils/http_client.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";
constexpr int kNativeContext = 262144;

// Registering a 62 GiB checkpoint with the GPU can take longer than
// the generic backend startup deadline.
constexpr long kStartupTimeoutSeconds = 1800;

class HalogenOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        (void)ctx;
        info.max_context_window = kNativeContext;
    }

    // Halogen sizes its KV pool itself and maps the checkpoint in place, so the
    // router's automatic context, which charges every .hgn file as resident
    // weights, would refuse loads that fit. An automatic ctx_size becomes the
    // native context before the router sees it.
    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        (void)info;
        const json ctx_size = options.get_option("ctx_size");
        if (!ctx_size.is_number_integer() || ctx_size.get<int>() <= 0) {
            options.set_option("ctx_size", kNativeContext);
        }
    }

    std::optional<utils::SetupFailure> check_container_host(
        const std::string& backend) const override {
        (void)backend;
        const std::string properties = kfd_gpu_properties("gfx1151");
        if (properties.empty()) return std::nullopt;
        return halogen::check_kfd_node(properties);
    }
};

std::map<std::string, unsigned long long> parse_kfd_properties(const std::string& properties) {
    std::map<std::string, unsigned long long> values;
    std::istringstream lines(properties);
    std::string key;
    unsigned long long value = 0;
    while (lines >> key >> value) {
        values[key] = value;
    }
    return values;
}

}  // namespace

namespace halogen {

std::optional<utils::SetupFailure> check_kfd_node(const std::string& properties) {
    auto values = parse_kfd_properties(properties);
    if ((values["capability"] & 0x08000000ULL) == 0) {
        return utils::SetupFailure{
            "kernel_svm", "capability & 0x08000000 is 0",
            {"Install Linux 6.18.4 or newer, built with CONFIG_HSA_AMD_SVM"}, ""};
    }
    const bool reported = values.count("cwsr_size") && values.count("ctl_stack_size") &&
                          values.count("simd_count");
    if (!reported || values["cwsr_size"] !=
                         values["ctl_stack_size"] + (values["simd_count"] / 2) * 479232ULL) {
        return utils::SetupFailure{
            "kernel_cwsr", "cwsr_size is not ctl_stack_size + (simd_count / 2) * 479232",
            {"Install Linux 6.18.4 or newer"}, ""};
    }
    return std::nullopt;
}

}  // namespace halogen

HalogenServer::HalogenServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("halogen", log_level, model_manager, backend_manager) {}

HalogenServer::~HalogenServer() {
    unload();
}

void HalogenServer::load(const std::string& model_name, const ModelInfo& model_info,
                         const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pinned image is present
    backend_manager_->install_backend(halogen::descriptor.recipe, kBackend);

    const std::string checkpoint = model_info.resolved_path("main");
    if (checkpoint.empty() || !fs::exists(checkpoint)) {
        throw std::runtime_error("halogen: HGN checkpoint not found for model '" + model_name +
                                 "' (checkpoint: " + model_info.checkpoint() + ")");
    }

    const std::string lookup_table = model_info.resolved_path("lookup_table");
    const std::string tokenizer =
        fs::path(model_info.resolved_path("tokenizer")).parent_path().string();
    const std::string vision = model_info.resolved_path("vision_tower");

    const json ctx_option = options.get_option("ctx_size");
    const int ctx_size = ctx_option.is_number_integer() && ctx_option.get<int>() > 0
                             ? ctx_option.get<int>()
                             : kNativeContext;

    device_type_ = DEVICE_GPU;
    port_ = choose_port();

    ServerCommand command;
    command.model_files = {checkpoint, lookup_table, tokenizer, vision};
    command.env = {
        {"HALOGEN_CHECKPOINT", checkpoint},
        {"HALOGEN_NGRAM_TABLE", lookup_table},
        {"HALOGEN_TOKENIZER", tokenizer},
    };
    if (!vision.empty()) {
        command.env.push_back({"HALOGEN_VISION_TOWER", vision});
    }
    command.env.insert(command.env.end(), {
        {"HALOGEN_API_PORT", std::to_string(port_)},
        {"HALOGEN_CTX", std::to_string(ctx_size)},
    });
    command.port = port_;
    command.ready_endpoint = "/health";

    LOG(INFO, "Halogen") << "Starting " << model_name << " on port " << port_ << std::endl;
    const bool inherit_output = (log_level_ == "info") || is_debug();
    start_server(std::make_unique<ContainerProcess>(
                     ProcessOutput{inherit_output, true}, halogen::descriptor.recipe, kBackend,
                     model_name, *halogen::descriptor.container_for(kBackend),
                     BackendUtils::get_backend_image(halogen::descriptor.recipe, kBackend)),
                 command,
                 kStartupTimeoutSeconds);
}

void HalogenServer::unload() {
    stop_server();
}

json HalogenServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json HalogenServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json HalogenServer::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

namespace halogen {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<HalogenServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

const BackendOps* ops() {
    return single_ops<HalogenOps>();
}

}  // namespace halogen
}  // namespace backends
}  // namespace lemon
