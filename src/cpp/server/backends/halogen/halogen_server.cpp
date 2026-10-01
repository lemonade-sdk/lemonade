#include "lemon/backends/halogen/halogen_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/halogen/halogen.h"
#include "lemon/model_manager.h"
#include "lemon/utils/http_client.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
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
constexpr const char* kTokenizerDir = "tokenizer";

// Registering a 115 GiB checkpoint with the GPU is bounded by storage
// bandwidth, not by anything global_timeout describes, so it gets its own
// floor; a larger global_timeout still raises it.
constexpr long kStartupTimeoutSeconds = 1800;

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::map<std::string, unsigned long long> parse_properties(const std::string& properties) {
    std::map<std::string, unsigned long long> values;
    std::istringstream lines(properties);
    std::string key;
    unsigned long long value = 0;
    while (lines >> key >> value) {
        values[key] = value;
    }
    return values;
}

// The properties of the gfx1151 KFD node, or "" when there is none.
std::string gfx1151_kfd_properties() {
    std::error_code ec;
    for (const auto& node :
         fs::directory_iterator("/sys/devices/virtual/kfd/kfd/topology/nodes", ec)) {
        std::ifstream file(node.path() / "properties");
        const std::string properties((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
        if (parse_properties(properties)["gfx_target_version"] == 110501) {
            return properties;
        }
    }
    return "";
}

class HalogenOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        (void)ctx;
        info.max_context_window = kNativeContext;
    }

    // An HGN bundle is the checkpoint, its overlays, the vision tower and the
    // tokenizer directory. Every Halogen model names the same checkpoint and
    // picks its overlay at launch, so one download serves all of them.
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant, const std::vector<std::string>& repo_files) const override {
        if (!ends_with(main_variant, ".hgn")) {
            return std::nullopt;
        }
        std::vector<std::string> selected;
        for (const auto& file : repo_files) {
            if (ends_with(file, ".hgn") || file.rfind(std::string(kTokenizerDir) + "/", 0) == 0) {
                selected.push_back(file);
            }
        }
        if (std::find(selected.begin(), selected.end(), main_variant) == selected.end()) {
            throw std::runtime_error("Halogen checkpoint not found in repository: " + main_variant);
        }
        return selected;
    }

    std::optional<utils::SetupFailure> check_container_host(
        const std::string& backend) const override {
        (void)backend;
        const std::string properties = gfx1151_kfd_properties();
        if (properties.empty()) {
            return std::nullopt;
        }
        return halogen::check_kfd_node(properties);
    }
};

}  // namespace

namespace halogen {

std::optional<utils::SetupFailure> check_kfd_node(const std::string& properties) {
    auto values = parse_properties(properties);
    if ((values["capability"] & 0x08000000ULL) == 0) {
        return utils::SetupFailure{
            "The kernel was built without SVM (KFD capability & 0x08000000 is 0)",
            "Install Linux 6.18.4 or newer, built with CONFIG_HSA_AMD_SVM"};
    }
    const bool reported = values.count("cwsr_size") && values.count("ctl_stack_size") &&
                          values.count("simd_count");
    if (!reported || values["cwsr_size"] !=
                         values["ctl_stack_size"] + (values["simd_count"] / 2) * 479232ULL) {
        return utils::SetupFailure{
            "The kernel lacks the gfx1151 fixes (KFD cwsr_size is not ctl_stack_size + "
            "(simd_count / 2) * 479232)",
            "Install Linux 6.18.4 or newer"};
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

    // The overlay, the tokenizer and the optional vision tower sit beside the
    // checkpoint in the same snapshot.
    const fs::path bundle = fs::path(checkpoint).parent_path();
    const std::string overlay_name = model_info.extra<std::string>("halogen_overlay", "");
    if (overlay_name.empty()) {
        throw std::runtime_error("halogen: model '" + model_name + "' names no halogen_overlay");
    }
    const std::string overlay = (bundle / overlay_name).string();
    const std::string tokenizer = (bundle / kTokenizerDir).string();
    const std::string vision_name = model_info.extra<std::string>("halogen_vision_tower", "");
    const std::string vision = vision_name.empty() ? "" : (bundle / vision_name).string();
    for (const auto& required : {overlay, tokenizer, vision}) {
        if (!required.empty() && !fs::exists(required)) {
            throw std::runtime_error("halogen: the bundle for '" + model_name + "' is missing " +
                                     required);
        }
    }

    const json ctx_option = options.get_option("ctx_size");
    const int ctx_size = (!ctx_size_is_auto() && ctx_option.is_number_integer() &&
                          ctx_option.get<int>() > 0)
                             ? ctx_option.get<int>()
                             : kNativeContext;
    set_started_ctx_size(ctx_size);

    device_type_ = DEVICE_GPU;
    port_ = choose_port();

    ServerCommand command;
    command.model_files = {checkpoint, overlay, tokenizer, vision};
    command.env = {
        {"HALOGEN_CHECKPOINT", checkpoint},
        {"HALOGEN_CK_OVERLAY", overlay},
        {"HALOGEN_TOKENIZER", tokenizer},
    };
    if (!vision.empty()) {
        command.env.push_back({"HALOGEN_VISION_TOWER", vision});
    }
    command.env.insert(command.env.end(), {
        {"HALOGEN_API_PORT", std::to_string(port_)},
        {"HALOGEN_CTX", std::to_string(ctx_size)},
        {"HALOGEN_KV_POOL_POSITIONS", "524288"},
        {"HALOGEN_KV_SLOTS", "4"},
        {"HALOGEN_PROMPT_CACHE", "2"},
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
                 (std::max)(kStartupTimeoutSeconds, utils::HttpClient::get_default_timeout()));
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
