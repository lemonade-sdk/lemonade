#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/ds4/ds4.h"
#include "lemon/auto_tune.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/gguf_reader.h"
#include "lemon/model_manager.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {

// The larger of the integrated GPU's carve-out and its GTT window, in GiB, or
// 0 when there is no integrated AMD GPU.
double igpu_pool_gb() {
    try {
        const GPUInfo igpu = create_system_info()->get_amd_igpu_device();
        if (igpu.available) {
            return (std::max)(igpu.vram_gb, igpu.virtual_gb);
        }
    } catch (...) {
    }
    return 0.0;
}

double file_gib(const std::string& path) {
    std::error_code ec;
    const auto bytes = path.empty() ? 0 : fs::file_size(utils::path_from_utf8(path), ec);
    return ec ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

// ds4-server takes reasoning_effort and chat_template_kwargs, which Lemonade
// sets beside the `/no_think` prefix it adds for templates that ignore them.
// DeepSeek reads the prefix as part of the user's message: with it, the
// resident Vision model answered a tool request with "Let me try again" and ran
// out of tokens before closing the call.
json without_no_think_prefix(json request) {
    static const std::string kPrefix = "/no_think\n";
    if (!request.contains("messages") || !request["messages"].is_array()) return request;
    for (auto& message : request["messages"]) {
        if (message.is_object() && message.value("role", "") == "user" &&
            message.contains("content") && message["content"].is_string()) {
            const std::string content = message["content"].get<std::string>();
            if (content.rfind(kPrefix, 0) == 0) message["content"] = content.substr(kPrefix.size());
        }
    }
    return request;
}

// The weights ds4 holds resident: the model and, when it has one, its DSpark
// support file.
double weights_gib(const ModelInfo& info) {
    return file_gib(info.resolved_path("main")) + file_gib(info.resolved_path("draft"));
}

}  // namespace

Ds4Server::Ds4Server(const std::string& log_level, ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer("ds4-server", log_level, model_manager, backend_manager) {
}

void Ds4Server::load(const std::string& model_name, const ModelInfo& model_info,
                     const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;  // install_backend() is a no-op once the pin is present

    std::string ds4_args = options.get_option("ds4_args");
    int ctx_size = options.get_option("ctx_size");

    // ds4-server only runs its own curated GGUFs. Accept either a
    // Hugging-Face-resolved local path or an absolute path used directly as
    // the checkpoint (user_models.json registrations of an existing file).
    std::string gguf_path = model_info.resolved_path("main");
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        const std::string checkpoint = model_info.checkpoint();
        if (!checkpoint.empty() && fs::path(checkpoint).is_absolute() && fs::exists(checkpoint)) {
            gguf_path = checkpoint;
        }
    }
    if (gguf_path.empty() || !fs::exists(gguf_path)) {
        throw std::runtime_error("ds4: no local GGUF found for model '" + model_name +
                                 "' (checkpoint: " + model_info.checkpoint() + ")");
    }

    // rocm is the only published variant, so there is no backend option to
    // read; ds4_args cannot select one either (see reserved_custom_arg_flags).
    const std::string backend = "rocm";
    device_type_ = DEVICE_GPU;

    backend_manager_->install_backend(ds4::spec()->recipe, backend);
    const std::string image = BackendUtils::get_backend_image(ds4::descriptor.recipe, backend);

    port_ = choose_port();

    std::vector<std::string> args;
    args.push_back("-m");
    args.push_back(gguf_path);
    args.push_back("--host");
    args.push_back("0.0.0.0");
    args.push_back("--port");
    args.push_back(std::to_string(port_));
    if (ctx_size > 0) {
        args.push_back("-c");
        args.push_back(std::to_string(ctx_size));
    }

    // A long prompt's default 4096-token prefill graph faults the GPU in a
    // quantize kernel and takes the server with it; chunked, it completes.
    args.push_back("--prefill-chunk");
    args.push_back("2048");
    const std::string draft_path = model_info.resolved_path("draft");
    const ds4::MemoryPlan plan = ds4::memory_plan(igpu_pool_gb(), weights_gib(model_info));
    if (plan.resident) {
        if (!draft_path.empty()) {
            args.insert(args.end(), {"--dspark", "--mtp-model", draft_path});
        }
    } else {
        // A user-supplied later flag (e.g. --ssd-streaming-cache-experts) still
        // wins since ds4-server parses left-to-right. DSpark needs residency.
        args.push_back("--ssd-streaming");
    }
    // A streaming ds4 grows into the memory free above its reserve, and with
    // less than its working set there it stalls without ever serving; a
    // resident one fails mid-load. Failing here instead lets the router evict
    // the other models and retry at once.
    const double working_set_gb = plan.resident ? weights_gib(model_info) : model_info.min_resident_gb;
    if (working_set_gb > 0.0 && plan.free_reserve_gb > 0) {
        bool memory_known = false;
        const double free_gb = get_available_memory_gb(
            DEVICE_GPU, gpu_memory_vendor_for_target(backend, ""), "", nullptr, &memory_known);
        const double needed_gb = plan.free_reserve_gb + working_set_gb;
        if (memory_known && free_gb < needed_gb) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(1) << "ds4 needs " << needed_gb
                    << " GiB of free GPU memory to start, and " << free_gb << " GiB is free";
            throw std::runtime_error(message.str());
        }
    }
    if (plan.expert_cache_gb > 0) {
        args.push_back("--ssd-streaming-cache-experts");
        args.push_back(std::to_string(plan.expert_cache_gb) + "GB");
    }

    if (!ds4_args.empty()) {
        const std::string validation_error =
            validate_custom_args(ds4_args, ds4::reserved_custom_arg_flags());
        if (!validation_error.empty()) {
            throw std::invalid_argument("Invalid custom ds4-server arguments:\n" + validation_error);
        }
        LOG(DEBUG, "DS4") << "Adding custom arguments: " << ds4_args << std::endl;
        std::vector<std::string> custom_args = parse_custom_args(ds4_args);
        args.insert(args.end(), custom_args.begin(), custom_args.end());
    }

    LOG(INFO, "DS4") << "Starting ds4-server (" << image << ") for " << gguf_path << " on port "
                     << port_ << std::endl;

    ServerCommand command;
    command.program = ds4::descriptor.binary;
    command.args = std::move(args);
    command.model_files = {gguf_path};
    if (plan.resident && !draft_path.empty()) {
        command.model_files.push_back(draft_path);
    }
    if (plan.model_cache_gb > 0) {
        command.env.push_back({"DS4_ROCM_STREAM_MODEL_CACHE_GB", std::to_string(plan.model_cache_gb)});
    }
    if (!plan.resident && plan.free_reserve_gb > 0) {
        command.env.push_back(
            {"DS4_ROCM_STREAM_FREE_RESERVE_GB", std::to_string(plan.free_reserve_gb)});
    }
    command.port = port_;
    // ds4-server binds its port only after the model is fully loaded, so first
    // reachability means ready. There is no /health endpoint; /v1/models is the
    // cheapest always-on route and doubles as the watchdog probe.
    command.ready_endpoint = "/v1/models";

    const bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    start_server(std::make_unique<ContainerProcess>(ProcessOutput{inherit_output, true},
                                                    ds4::descriptor.recipe, backend, model_name,
                                                    *ds4::descriptor.container_for(backend), image),
                 command, HttpClient::get_default_timeout());
}

json Ds4Server::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", without_no_think_prefix(request));
}

void Ds4Server::forward_streaming_request(const std::string& endpoint,
                                          const std::string& request_body,
                                          httplib::DataSink& sink, bool sse,
                                          long timeout_seconds,
                                          TelemetryCallback telemetry_callback) {
    std::string body = request_body;
    if (endpoint.find("chat/completions") != std::string::npos) {
        const json parsed = json::parse(request_body, nullptr, false);
        if (!parsed.is_discarded()) body = without_no_think_prefix(parsed).dump();
    }
    WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout_seconds,
                                             std::move(telemetry_callback));
}

json Ds4Server::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json Ds4Server::responses(const json& request) {
    return forward_request("/v1/responses", request);
}

namespace ds4 {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<Ds4Server>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary);
    return &kSpec;
}

namespace {

class Ds4Ops : public BackendOps {
public:
    // ds4 models are GGUFs with standard <arch>.* keys, which give auto-tune
    // the real context window and KV shape instead of a size-based guess. The
    // capability labels stay as registered: a GGUF's MTP layer, for one, is
    // nothing ds4-server can run.
    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        const std::string path = info.resolved_path();
        std::error_code ec;
        if (path.empty() || !fs::exists(utils::path_from_utf8(path), ec)) return;
        GgufMetadata meta;
        if (!read_gguf_metadata(meta, path)) return;
        info.max_context_window = meta.context_length;
        info.gguf = std::move(meta);
    }

    // The context comes out of what is free above memory_plan's reserve, after
    // the resident weights or, when streaming, the working set the model needs
    // to run at speed.
    std::optional<double> auto_context_kv_budget_gib(const ModelInfo& info,
                                                     double available_gib) const override {
        const MemoryPlan plan = memory_plan(igpu_pool_gb(), weights_gib(info));
        const double working_set_gb = plan.resident ? weights_gib(info) : info.min_resident_gb;
        if (working_set_gb <= 0.0) return std::nullopt;
        return available_gib - plan.free_reserve_gb - working_set_gb;
    }
};

}  // namespace

const BackendOps* ops() {
    static const Ds4Ops kOps;
    return &kOps;
}

}  // namespace ds4

}  // namespace backends
}  // namespace lemon
