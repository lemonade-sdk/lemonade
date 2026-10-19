#include "lemon/backends/hrx/hrx.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "llm_configured_fixture.h"

#include <filesystem>

using namespace llm_oracle;

void* operator new(size_t size) { if (track_allocations) ++allocations; if (void* memory = std::malloc(size ? size : 1)) return memory; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
const std::string k_sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\ndata: [DONE]\n\n";

class Upstream {
public:
    Upstream() {
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Get("/slots", [this](const auto& request, auto& response) { record(request); response.set_content(R"({"slots":[2]})", "application/json"); });
        for (const char* path : {"/v1/chat/completions", "/v1/completions", "/v1/responses", "/v1/embeddings", "/v1/rerank", "/tokenize", "/slots/2"}) {
            http.Post(path, [this](const auto& request, auto& response) {
                record(request);
                if (request.body.find("stream") != std::string::npos) response.set_content(k_sse, "text/event-stream");
                else response.set_content(R"({"model":"/models/qualified.gguf","witness":true})", "application/json");
            });
        }
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Fake loopback bind failed");
        thread = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~Upstream() { http.stop(); thread.join(); }
    void record(const httplib::Request& request) {
        std::lock_guard<std::mutex> lock(mutex);
        method = request.method; target = request.target; body = request.body;
    }
    std::string last(const char* field) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::string(field) == "method" ? method : std::string(field) == "target" ? target : body;
    }
    httplib::Server http;
    int port = 0;
private:
    std::thread thread;
    std::mutex mutex;
    std::string method, target, body;
};

struct Arm {
    Fixture fixture;
    std::shared_ptr<Services> services;
    std::unique_ptr<WrappedServer> server;
    Arm(int port, const std::string& level = "error", PlatformInfo host = {"linux", "x86_64", ""}) : fixture(port) {
        fixture.executable = "/bundle/hrx/llama-server";
        services = std::make_shared<Services>(fixture);
        services->host = host;
        server = std::make_unique<ConfiguredServer>(hrx::configured_entry(), BackendContext{level}, ConfiguredServerServices{services, fixture.effects().make_process});
    }
};

ModelInfo hrx_model(const std::string& path = "/models/qualified.gguf") {
    ModelInfo model;
    model.checkpoints["main"] = "fixture/repo";
    model.resolved_paths["main"] = path;
    return model;
}

RecipeOptions hrx_options(const json& args = nullptr, int context = 32768) {
    json options = {{"ctx_size", context}};
    if (!args.is_null()) options["hrx_args"] = args;
    return RecipeOptions("llamacpp-hrx", options);
}

std::string load(Arm& arm, const RecipeOptions& options = hrx_options(), const ModelInfo& model = hrx_model()) {
    arm.server->set_model_metadata("fixture", "fixture/repo", ModelType::LLM, DEVICE_NPU, options);
    try { arm.server->load("fixture", model, options, false); }
    catch (const std::exception& error) { return error.what(); }
    arm.server->set_state(ModelState::READY);
    return "";
}

std::vector<std::string> argv(int port, std::vector<std::string> tail) {
    std::vector<std::string> result = {"-m", "/models/qualified.gguf", "--ctx-size", "32768", "--device", "HRX0", "--port", std::to_string(port), "--jinja", "--metrics"};
    result.insert(result.end(), tail.begin(), tail.end());
    return result;
}

void check_launch(Upstream& upstream) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"--threads 7 --load-mode none", {"--threads", "7", "--load-mode", "none", "--parallel", "1"}},
        {"", {"--parallel", "1"}},
        {"-np 2", {"-np", "2"}},
        {"--parallel 3", {"--parallel", "3"}},
    };
    for (const auto& [args, tail] : cases) {
        Arm arm(upstream.port);
        check(load(arm, hrx_options(args.empty() ? json() : json(args))).empty(), "HRX loads through the configured server");
        check(arm.fixture.command.program == "/bundle/hrx/llama-server" && arm.fixture.command.args == argv(upstream.port, tail), "HRX builds the complete managed argv and --parallel default: " + args);
        check(arm.fixture.command.env == std::vector<std::pair<std::string, std::string>>{{"GGML_DISABLE_VULKAN", "1"}}, "HRX builds the exact process environment");
        check(arm.fixture.events == std::vector<std::string>{"install:llamacpp-hrx:hrx", "port", "managed:llamacpp-hrx:hrx", "process", "start"}, "HRX install, port, managed lookup and start order without an existence probe");
        check(arm.fixture.command.ready_endpoint == "/health" && arm.fixture.cwd.empty() && arm.fixture.output.filter_health_logs && arm.server->get_device_type() == DEVICE_NPU, "HRX readiness, cwd, health-log filter and untouched device");
    }
    for (const char* level : {"info", "debug", "trace", "warning", "error"}) {
        Arm arm(upstream.port, level);
        load(arm);
        const std::string value = level;
        check(arm.fixture.output.inherit == (value == "info" || value == "debug" || value == "trace"), "HRX inherits child output at info and debug levels only: " + value);
    }
    {
        Arm arm(upstream.port);
        check(load(arm, hrx_options(), hrx_model("")) == "GGUF file not found for checkpoint: fixture/repo" && arm.fixture.events == std::vector<std::string>{"install:llamacpp-hrx:hrx"}, "missing GGUF fails after install and before the port");
    }
    {
        Arm arm(upstream.port);
        arm.fixture.child_running = false;
        check(load(arm) == "llama-server failed to start" && arm.fixture.stops == 1, "HRX keeps the llama-server process identity");
    }
    for (const auto& host : {PlatformInfo{"windows", "x86_64", ""}, PlatformInfo{"macos", "aarch64", ""}, PlatformInfo{"linux", "aarch64", ""}, PlatformInfo{"linux", "", ""}}) {
        Arm arm(upstream.port, "debug", host);
        LogCapture capture("HRX");
        check(load(arm) == "The HRX backend is available on Linux x86-64 only" && arm.fixture.events.empty() && capture.records.empty(), "HRX rejects a non-Linux-x86-64 host before logging or installing: " + host.os + "/" + host.machine_arch);
    }
}

void check_admission(Upstream& upstream) {
    const std::set<std::string> reserved = {"-c", "-dev", "-dr", "-hf", "-hff", "-hfr", "-m", "-mu", "--ctx-size", "--device", "--docker-repo",
        "--hf-file", "--hf-repo", "--jinja", "--metrics", "--model", "--model-url", "--no-jinja", "--port"};
    check(hrx::configured_entry()->definition->records["launch"]["custom_args"]["reserved"].get<std::set<std::string>>() == reserved, "HRX reserves exactly its managed flags and aliases");
    for (const auto& flag : reserved) {
        for (const auto& form : {flag + " replacement", flag + "=replacement"}) {
            Arm arm(upstream.port);
            const auto message = load(arm, hrx_options(form));
            check(message.rfind("Invalid custom HRX llama-server arguments:\n", 0) == 0 && message.size() > 44 &&
                  arm.fixture.events == std::vector<std::string>{"install:llamacpp-hrx:hrx", "port", "managed:llamacpp-hrx:hrx"}, "HRX rejects a managed argument after the managed lookup: " + form);
        }
    }
}

void check_install() {
    const auto* spec = hrx::spec();
    check(spec->recipe == "llamacpp-hrx" && spec->binary == "llama-server" && spec->pin_recipe == "llamacpp-hrx" && !spec->supports_split_archive, "HRX install identities");
    for (const auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
        const InstallContext context{*spec, spec->definition, "llamacpp-hrx", spec->pin_recipe, "hrx", "", "hrx-b99", "hrx-b99", {"linux", "x86_64", ""}, phase};
        const auto params = spec->install_params_fn(context);
        check(params.repo == "ROCm/ggml-staging-automation" && params.filename == "llama-hrx-b99-bin-manylinux-hrx-x64.tar.gz" && params.version_override.empty() && params.sha256.empty(), "HRX installer maps the version to the exact release asset");
    }
    const InstallContext split{*spec, spec->definition, "llamacpp-hrx", spec->pin_recipe, "hrx", "", "pinned", "resolved", {"linux", "x86_64", ""}, InstallPhase::Repository};
    check(spec->install_params_fn(split).filename == "llama-pinned-bin-manylinux-hrx-x64.tar.gz", "repository phase uses the pinned version");
    auto asset = split;
    asset.phase = InstallPhase::Asset;
    check(spec->install_params_fn(asset).filename == "llama-resolved-bin-manylinux-hrx-x64.tar.gz", "asset phase uses the resolved version");
    const std::vector<std::tuple<std::string, PlatformInfo, std::string>> rejected = {
        {"rocm", {"linux", "x86_64", ""}, "Unsupported HRX backend 'rocm'; expected 'hrx'"},
        {"rocm", {"windows", "x86_64", ""}, "Unsupported HRX backend 'rocm'; expected 'hrx'"},
        {"hrx", {"windows", "x86_64", ""}, "The HRX backend is available on Linux x86-64 only"},
        {"hrx", {"macos", "aarch64", ""}, "The HRX backend is available on Linux x86-64 only"},
        {"hrx", {"linux", "aarch64", ""}, "The HRX backend is available on Linux x86-64 only"},
    };
    for (const auto& [backend, platform, expected] : rejected) {
        const InstallContext context{*spec, spec->definition, "llamacpp-hrx", spec->pin_recipe, backend, "", "hrx-b99", "hrx-b99", platform, InstallPhase::Asset};
        std::string message;
        try { spec->install_params_fn(context); } catch (const std::exception& error) { message = error.what(); }
        check(message == expected, "HRX installer rejects backend before host: " + backend + "/" + platform.os + "/" + platform.machine_arch);
    }
    std::vector<std::string> probes;
    const auto platform = install_platform(*spec, "hrx", {[&] { probes.push_back("os"); return std::string("linux"); }, [&] { probes.push_back("arch"); return std::string("gfx1151"); }});
    check(probes == std::vector<std::string>{"os"} && platform.rocm_arch.empty(), "HRX installation never probes the ROCm architecture");
}

void check_logging(Upstream& upstream) {
    Arm arm(upstream.port, "debug");
    LogCapture capture("HRX");
    load(arm, hrx_options("--threads 7"));
    check(capture.records == std::vector<std::string>{"Info Loading model: fixture", "Debug Per-model settings: ctx_size=32768, hrx_args=--threads 7, merge_args=true, auto_evict=(none), evict_idle_timeout=300, downsize_idle_timeout=60, evict_weight_factor=1.000000, pinned=false, auto_update=(none)"}, "HRX keeps its loading and per-model settings lines and has no startup line");
}

void check_operations(Upstream& upstream) {
    Arm arm(upstream.port);
    load(arm);
    auto& server = *arm.server;
    for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) for (const auto mode : k_operation_model_types) {
        const bool expected = (delivery == 0 && (op <= 4 || op >= 15)) || (delivery == 1 && op <= 2);
        check(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected, "HRX operation matrix " + std::to_string(op) + "/" + std::to_string(delivery));
    }
    for (const auto& description : server.operation_bindings()->descriptions()) check(description.provenance == OperationProvenance::Implemented && description.model_types == k_all_operation_model_types, "HRX bindings are implemented for every mode");
    check(server.operation_bindings()->descriptions().size() == 11, "HRX binds eleven operation cells");
    const json tools = {{"tools", {{{"function", {{"parameters", {{"type", "string"}, {"maxLength", 5000}, {"minLength", 1}}}}}}}}};
    json request = tools;
    request["model"] = "client-fixture";
    request["max_completion_tokens"] = 7;
    const std::vector<std::tuple<BackendOperation, std::string, bool, bool>> buffered = {
        {BackendOperation::ChatCompletion, "/v1/chat/completions", true, true},
        {BackendOperation::Completion, "/v1/completions", true, false},
        {BackendOperation::Responses, "/v1/responses", false, true},
    };
    for (const auto& [operation, path, alias, sanitize] : buffered) {
        const auto response = server.dispatch_completion_operation(operation, request);
        const auto body = json::parse(upstream.last("body"));
        check(upstream.last("method") == "POST" && upstream.last("target") == path, "HRX buffered route " + path);
        check(response == json{{"model", "client-fixture"}, {"witness", true}}, "HRX rewrites the echoed model to the client-facing id on " + path);
        check(body.contains("max_tokens") == alias && (!alias || body["max_tokens"] == 7) && body["max_completion_tokens"] == 7, "HRX token alias stays operation-specific on " + path);
        check(body["tools"][0]["function"]["parameters"].contains("maxLength") == !sanitize && body["tools"][0]["function"]["parameters"]["minLength"] == 1, "HRX tool-schema sanitization stays operation-specific on " + path);
        auto anonymous = request;
        anonymous.erase("model");
        check(server.dispatch_completion_operation(operation, anonymous)["model"] == "fixture", "HRX falls back to the loaded model name without a request model on " + path);
    }
    const std::vector<std::tuple<BackendOperation, std::string, std::string, std::string>> streaming = {
        {BackendOperation::ChatCompletion, "/v1/chat/completions", "{ \"stream\" : true, \"max_completion_tokens\" : 7 }", R"({"max_completion_tokens":7,"max_tokens":7,"stream":true})"},
        {BackendOperation::Completion, "/v1/completions", "{ \"stream\" : true, \"max_completion_tokens\" : 7 }", "{ \"stream\" : true, \"max_completion_tokens\" : 7 }"},
        {BackendOperation::Responses, "/v1/responses", "{ \"stream\" : true, \"max_completion_tokens\" : 7 }", R"({"max_completion_tokens":7,"stream":true})"},
        {BackendOperation::ChatCompletion, "/v1/chat/completions", "{\"stream\": not json", "{\"stream\": not json"},
        {BackendOperation::Responses, "/v1/responses", "{\"stream\": not json", "{\"stream\": not json"},
    };
    for (const auto& [operation, path, input, expected] : streaming) {
        Capture output;
        server.dispatch_streaming_operation(operation, input, output.sink);
        check(upstream.last("target") == path && upstream.last("body") == expected && output.bytes == k_sse, "HRX streaming request codec on " + path + ": " + expected);
    }
    {
        Capture output;
        json streamed = tools;
        streamed["stream"] = true;
        server.dispatch_streaming_operation(BackendOperation::ChatCompletion, streamed.dump(), output.sink);
        check(!json::parse(upstream.last("body"))["tools"][0]["function"]["parameters"].contains("maxLength"), "HRX streaming chat sanitizes tool schemas");
    }
    const json plain = {{"model", "client-fixture"}, {"input", "text"}};
    operation_adapter<IEmbeddingsServer>(server, BackendOperation::Embeddings, OperationDelivery::Buffered, ModelType::LLM)->embeddings(plain);
    check(upstream.last("target") == "/v1/embeddings" && json::parse(upstream.last("body")) == plain, "HRX embeddings reach llama-server unchanged despite the narrow advertised mask");
    const auto reranked = operation_adapter<IRerankingServer>(server, BackendOperation::Reranking, OperationDelivery::Buffered, ModelType::LLM)->reranking(plain);
    check(upstream.last("target") == "/v1/rerank" && reranked["model"] == "/models/qualified.gguf", "HRX reranking reaches llama-server without model normalization");
    auto* slots = operation_adapter<ISlotsServer>(server, BackendOperation::Slots, OperationDelivery::Buffered, ModelType::LLM);
    check(slots && slots->get_slots() == json{{"slots", {2}}} && upstream.last("method") == "GET" && upstream.last("target") == "/slots", "HRX slots retain GET dispatch");
    operation_adapter<ISlotsServer>(server, BackendOperation::SlotAction, OperationDelivery::Buffered, ModelType::LLM)->slots_action(2, "save", plain);
    check(upstream.last("method") == "POST" && upstream.last("target") == "/slots/2?action=save" && json::parse(upstream.last("body")) == plain, "HRX slot action retains POST path, action query and body");
    operation_adapter<ITokenizerServer>(server, BackendOperation::Tokenize, OperationDelivery::Buffered, ModelType::LLM)->tokenize(plain);
    check(upstream.last("target") == "/tokenize" && json::parse(upstream.last("body")) == plain, "HRX tokenize retains POST dispatch");
}

void check_registration() {
    const auto entry = hrx::configured_entry();
    descriptor(entry->descriptor, hrx::descriptor, true);
    const auto& d = entry->descriptor;
    check(d.recipe == "llamacpp-hrx" && d.display_name == "HRX GPU" && d.binary == "llama-server" && d.config_section == "hrx" && d.default_device == DEVICE_GPU && d.uses_ctx_size && d.exposes_prometheus_metrics && d.takes_args && d.bin_variants == std::vector<std::string>{"hrx"}, "independent HRX descriptor identities");
    check(d.support.size() == 1 && d.support[0].arch_gates.size() == 2 && d.support[0].arch_gates.at("gfx1100").os == std::set<std::string>{"linux"} && d.support[0].arch_gates.at("gfx1151").channels.empty() &&
          hrx::descriptor.support[0].arch_gates.size() == 2 && hrx::descriptor.support[0].arch_gates.at("gfx1151").os == std::set<std::string>{"linux"}, "HRX arch gates match the compiled descriptor");
    check(entry->compatibility == TrustedCompatibility::None && !entry->launch_compatibility.device_on_backend_selection && !entry->launch_compatibility.posix_executable_loader_path && entry->launch_compatibility.load_log, "HRX carries only the load-log attachment");
    check(hrx::capabilities() == 0 && hrx::capabilities() == (llamacpp::capabilities() & ~(CAP_EMBEDDINGS | CAP_RERANKING)), "HRX advertised capability mask stays chat-only");
    const auto* ops = hrx::ops();
    const auto root = std::filesystem::temp_directory_path() / ("hrx-oracle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "snapshots/abc");
    std::filesystem::create_directories(root / "refs");
    std::ofstream(root / "refs/main") << "abc";
    std::ofstream(root / "snapshots/abc/model-Q4_K_M.gguf") << "GGUF";
    std::ofstream(root / "snapshots/abc/corrupt.gguf") << "JUNK";
    CheckpointResolveContext context;
    context.model_cache_path = root.string();
    context.type = "main";
    context.variant = "Q4_K_M";
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == (root / "snapshots/abc/model-Q4_K_M.gguf").string(), "HRX resolves the GGUF variant through llama.cpp checkpoint rules");
    check(ops->validate_checkpoint_file((root / "snapshots/abc/model-Q4_K_M.gguf").string()).empty() && ops->validate_checkpoint_file((root / "snapshots/abc/corrupt.gguf").string()) == "Invalid GGUF cache file", "HRX validates GGUF magic");
    check(ops->validate_registration_checkpoint("Owner/Model-GGUF").rfind("You are required to provide a 'variant'", 0) == 0 && ops->validate_registration_checkpoint("Owner/Model-GGUF:Q4_0").empty(), "HRX requires a GGUF registration variant");
    ModelInfo info;
    info.resolved_paths["main"] = (root / "snapshots/abc/corrupt.gguf").string();
    ops->populate_metadata(info, {});
    check(info.max_context_window == ModelInfo{}.max_context_window, "HRX metadata probing ignores an unreadable GGUF");
    RecipeOptions runtime("llamacpp-hrx", {{"hrx_args", "--threads 7"}});
    ops->resolve_runtime_options(ModelInfo{}, runtime);
    check(runtime.get_option("hrx_args") == "--threads 7" && ops->resolve_version("system", "file") == "file" && ops->check_install("system", true).installed, "HRX keeps the default runtime-option, version and install checks");
    std::filesystem::remove_all(root);
}
}

int main() {
    Upstream upstream;
    check_registration();
    check_install();
    check_launch(upstream);
    check_admission(upstream);
    check_logging(upstream);
    check_operations(upstream);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
