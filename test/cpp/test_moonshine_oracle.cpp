#include "lemon/backends/moonshine/moonshine.h"
#include "lemon/backends/moonshine/moonshine_server.h"
#include "llm_configured_fixture.h"

using namespace llm_oracle;

void* operator new(size_t size) { if (track_allocations) ++allocations; if (void* memory = std::malloc(size ? size : 1)) return memory; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
const std::string k_sse = "data: {\"fixture\":true}\n\ndata: [DONE]\n\n";
const std::string k_model = u8"/models/moonshine/雪 medium";

class Upstream {
public:
    Upstream() {
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Post("/inference", [this](const auto& request, auto& response) {
            std::lock_guard<std::mutex> lock(mutex);
            form = request.form; ++count; target = request.target;
            response.status = status;
            response.set_content(reply, "application/json");
        });
        for (const char* path : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
            http.Post(path, [this](const auto& request, auto& response) {
                std::lock_guard<std::mutex> lock(mutex);
                target = request.target; body = request.body; ++count;
                response.set_content(k_sse, "text/event-stream");
            });
        }
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Fake loopback bind failed");
        thread = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~Upstream() { http.stop(); thread.join(); }
    void respond(int code, std::string bytes) { std::lock_guard<std::mutex> lock(mutex); status = code; reply = std::move(bytes); }
    std::string field(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        if (form.has_file(name)) return form.get_file(name).content;
        return form.has_field(name) ? form.get_field(name) : "<absent>";
    }
    std::string file(const char* part) {
        std::lock_guard<std::mutex> lock(mutex);
        return std::string(part) == "filename" ? form.get_file("file").filename : form.get_file("file").content_type;
    }
    std::string last_target() { std::lock_guard<std::mutex> lock(mutex); return target; }
    std::string last_body() { std::lock_guard<std::mutex> lock(mutex); return body; }
    size_t requests() { std::lock_guard<std::mutex> lock(mutex); return count; }
    httplib::Server http;
    int port = 0;
private:
    std::thread thread;
    std::mutex mutex;
    httplib::MultipartFormData form;
    std::string target, body, reply = R"({"text":"fixture transcription"})";
    int status = 200;
    size_t count = 0;
};

struct Arm {
    Fixture fixture;
    std::set<int> free;
    std::unique_ptr<WrappedServer> server;
    Arm(int port, const std::string& level = "error") : fixture(port), free{port, port + 1, port + 2} {
        fixture.executable = "/bundle/moonshine/moonshine-server";
        fixture.paths = {k_model};
        auto effects = fixture.effects();
        effects.find_free_port = [this](int start) {
            fixture.events.push_back("probe:" + std::to_string(start));
            const auto found = free.lower_bound(start);
            return found == free.end() ? -1 : *found;
        };
        server = std::make_unique<ConfiguredServer>(moonshine::configured_entry(), BackendContext{level}, ConfiguredServerServices{std::make_shared<ProbeServices>(effects), fixture.effects().make_process});
    }
    struct ProbeServices : EffectLaunchServices {
        explicit ProbeServices(BackendEffects effects) : EffectLaunchServices(std::move(effects)) {}
        PlatformInfo platform() const override { return {"linux", "x86_64", ""}; }
    };
    std::string load(const ModelInfo& model, const RecipeOptions& options = RecipeOptions("moonshine", json::object())) {
        server->set_model_metadata("fixture", "fixture/repo", ModelType::TRANSCRIPTION, DEVICE_NPU, options);
        try { server->load("fixture", model, options, false); }
        catch (const std::exception& error) { return error.what(); }
        server->set_state(ModelState::READY);
        return "";
    }
};

ModelInfo model(const std::string& checkpoint = "UsefulSensors/moonshine-streaming:medium-streaming-en/quantized", json arch = json()) {
    ModelInfo info;
    info.checkpoints["main"] = checkpoint;
    info.resolved_paths["main"] = k_model;
    if (!arch.is_null()) info.extras["moonshine_arch"] = arch;
    return info;
}

std::string address(WrappedServer& server) {
    return operation_adapter<IStreamingTranscriptionServer>(server, BackendOperation::StreamingTranscription, OperationDelivery::Realtime, ModelType::TRANSCRIPTION)->get_streaming_address();
}

std::vector<std::string> probes(const std::vector<std::string>& events) {
    std::vector<std::string> result;
    for (const auto& event : events) if (event.rfind("probe:", 0) == 0) result.push_back(event);
    return result;
}

void check_launch(Upstream& upstream) {
    const int u = upstream.port;
    {
        Arm arm(u);
        check(address(*arm.server).empty(), "Moonshine reports no streaming address before load");
        check(arm.load(model()).empty(), "Moonshine loads through the configured server");
        check(arm.fixture.command.program == "/bundle/moonshine/moonshine-server" && arm.fixture.command.args == std::vector<std::string>{"--model-path", k_model, "--model-arch", "5", "--port", std::to_string(u), "--tcp-port", std::to_string(u + 2)}, "Moonshine argv with consecutive HTTP and TCP ports");
        check(arm.fixture.command.env == std::vector<std::pair<std::string, std::string>>{{"PYTHONNOUSERSITE", "1"}}, "Moonshine isolates the bundled Python environment");
        check(arm.fixture.events == std::vector<std::string>{"install:moonshine:cpu", "exists:" + k_model, "managed:moonshine:cpu", "probe:8001", "probe:" + std::to_string(u + 1), "probe:" + std::to_string(u + 2), "process", "start"}, "Moonshine install, model check, lookup and three-port probe order");
        check(arm.fixture.command.ready_endpoint == "/health" && arm.fixture.cwd.empty() && !arm.fixture.output.filter_health_logs && arm.server->get_device_type() == DEVICE_CPU, "Moonshine readiness, cwd, unfiltered output and CPU device");
        check(address(*arm.server) == "tcp://127.0.0.1:" + std::to_string(u + 2), "Moonshine streaming address names the TCP port");
    }
    {
        Arm arm(u);
        arm.free = {u - 3, u - 2, u, u + 1, u + 2};
        check(arm.load(model()).empty() && probes(arm.fixture.events) == std::vector<std::string>{"probe:8001", "probe:" + std::to_string(u - 2), "probe:" + std::to_string(u - 1),
            "probe:" + std::to_string(u - 2), "probe:" + std::to_string(u - 1), "probe:" + std::to_string(u - 1), "probe:" + std::to_string(u + 1), "probe:" + std::to_string(u + 2)}, "Moonshine retries from the next candidate until three ports are free");
    }
    {
        Arm arm(u);
        arm.free.clear();
        for (int port = 8001; port < 8001 + 200; port += 2) arm.free.insert(port);
        check(arm.load(model()) == "Failed to find three consecutive available ports" && probes(arm.fixture.events).size() == 100 && arm.fixture.events.back() == "probe:8100", "Moonshine gives up after fifty attempts");
        Arm zero(u);
        zero.free = {0};
        check(zero.load(model()) == "Failed to find three consecutive available ports" && probes(zero.fixture.events) == std::vector<std::string>{"probe:8001", "probe:0", "probe:1", "probe:0"}, "Moonshine stops once the probe returns port zero");
        Arm none(u);
        none.free.clear();
        std::vector<std::string> expected = {"probe:8001", "probe:0"};
        for (int attempt = 1; attempt < 50; ++attempt) expected.insert(expected.end(), {"probe:0", "probe:0"});
        check(none.load(model()) == "Failed to find three consecutive available ports" && probes(none.fixture.events) == expected, "Moonshine keeps the wrapper's retry sequence when no port is free");
    }
    for (const char* level : {"info", "debug", "trace", "warning", "error"}) {
        Arm arm(u, level);
        arm.load(model());
        const std::string value = level;
        check(arm.fixture.output.inherit == (value == "info" || value == "debug" || value == "trace"), "Moonshine inherits child output at info and debug: " + value);
    }
    const std::vector<std::tuple<std::string, json, std::string>> architectures = {
        {"Org/Moonshine-TINY", json(), "2"}, {"org/moonshine:small-streaming-en", json(), "4"}, {"org/tiny-small", json(), "2"}, {"org/base", json(), "5"},
        {"org/tiny", 3, "3"}, {"org/tiny", 3.9, "3"}, {"org/tiny", "x", "2"}, {"org/small", nullptr, "4"}, {"org/small", -1, "4"}, {"org/small", 0, "0"}, {"org/small", true, "1"},
    };
    for (const auto& [checkpoint, arch, expected] : architectures) {
        Arm arm(u);
        arm.load(model(checkpoint, arch));
        check(arm.fixture.command.args[3] == expected, "Moonshine architecture for " + checkpoint + " / " + arch.dump());
    }
    {
        Arm arm(u);
        arm.fixture.paths.clear();
        check(arm.load(model()) == "Model directory not found for checkpoint: UsefulSensors/moonshine-streaming:medium-streaming-en/quantized" &&
              arm.fixture.events == std::vector<std::string>{"install:moonshine:cpu", "exists:" + k_model} && arm.server->get_device_type() == DEVICE_CPU, "missing model fails after install with the CPU device already set");
        Arm failing(u);
        failing.fixture.install_fails = true;
        check(failing.load(model()) == "install witness" && failing.server->get_device_type() == DEVICE_CPU, "the CPU device is set before installation");
        Arm exited(u);
        exited.fixture.child_running = false;
        check(exited.load(model()) == "moonshine-server failed to start" && exited.fixture.stops == 1, "Moonshine keeps the moonshine-server process identity");
    }
}

void check_failure_states(Upstream& upstream) {
    const int u = upstream.port;
    const auto tcp = [](int port) { return "tcp://127.0.0.1:" + std::to_string(port); };
    const RecipeOptions invalid("moonshine", {{"moonshine_args", "--port 1"}});
    const std::string rejected = "Invalid custom moonshine-server arguments:\n";
    {
        Arm arm(u);
        check(arm.load(model(), invalid).rfind(rejected, 0) == 0 && probes(arm.fixture.events).size() == 3, "Moonshine rejects custom arguments after probing all three ports");
        check(address(*arm.server).empty() && arm.server->get_backend_port() == 0, "a fresh Moonshine object that fails preparation reports no TCP address and no HTTP port");
    }
    {
        Arm arm(u);
        arm.fixture.child_running = false;
        check(arm.load(model()) == "moonshine-server failed to start", "Moonshine readiness failure");
        check(address(*arm.server) == tcp(u + 2) && arm.server->get_backend_port() == 0, "a Moonshine readiness failure keeps the newly mapped TCP address while the HTTP port resets");
    }
    {
        Arm arm(u);
        check(arm.load(model()).empty() && address(*arm.server) == tcp(u + 2) && arm.server->get_backend_port() == u, "Moonshine loads before the failed reloads");
        arm.free = {u - 3, u - 2, u - 1};
        check(arm.load(model(), invalid).rfind(rejected, 0) == 0 && probes(arm.fixture.events).back() == "probe:" + std::to_string(u - 1), "a reload probes new ports before rejecting its custom arguments");
        check(address(*arm.server) == tcp(u + 2) && arm.server->get_backend_port() == u, "a failed Moonshine preparation keeps the previous TCP address and HTTP port");
        arm.free.clear();
        check(arm.load(model()) == "Failed to find three consecutive available ports", "Moonshine reload port search fails");
        check(address(*arm.server) == tcp(u + 2) && arm.server->get_backend_port() == u, "a failed Moonshine port search keeps the previous TCP address and HTTP port");
    }
}

void check_logging(Upstream& upstream) {
    const int u = upstream.port;
    {
        Arm arm(u, "debug");
        LogCapture capture("MoonshineServer");
        const RecipeOptions options("moonshine", {{"moonshine_args", "--threads 2"}});
        arm.load(model(), options);
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture",
            "Info Per-model settings: moonshine_args=--threads 2, merge_args=true, auto_evict=(none), evict_idle_timeout=300, downsize_idle_timeout=60, evict_weight_factor=1.000000, pinned=false, auto_update=(none)", "Info Using model: " + k_model,
            "Info Using executable: /bundle/moonshine/moonshine-server", "Info Starting server on port " + std::to_string(u) + " (TCP streaming on " + std::to_string(u + 2) + ")",
            "Debug Adding custom arguments: --threads 2"}, "Moonshine keeps its load diagnostics in order");
        check(arm.fixture.command.args.size() == 10 && arm.fixture.command.args[8] == "--threads" && arm.fixture.command.args[9] == "2", "Moonshine appends custom arguments after the managed argv");
    }
    for (const char* flag : {"--model-path", "--model-arch", "--port", "--tcp-port"}) for (const char* suffix : {" x", "=x"}) {
        Arm arm(u, "debug");
        LogCapture capture("MoonshineServer");
        const auto message = arm.load(model(), RecipeOptions("moonshine", {{"moonshine_args", std::string(flag) + suffix}}));
        check(message.rfind("Invalid custom moonshine-server arguments:\n", 0) == 0 && std::find(arm.fixture.events.begin(), arm.fixture.events.end(), "process") == arm.fixture.events.end() &&
              capture.records.size() == 5 && capture.records[4].rfind("Info Starting server on port", 0) == 0, std::string("Moonshine rejects a managed flag after the startup line: ") + flag + suffix);
    }
}

void check_transcription(Upstream& upstream) {
    Arm arm(upstream.port);
    arm.load(model());
    auto& server = *arm.server;
    for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) for (const auto mode : k_operation_model_types) {
        const bool expected = (op <= 2 && delivery == 1) || (op == 6 && delivery == 0) || (op == 7 && delivery == 2);
        check(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected, "Moonshine operation matrix " + std::to_string(op) + "/" + std::to_string(delivery));
    }
    for (const auto& description : server.operation_bindings()->descriptions()) {
        check((description.provenance == OperationProvenance::LegacyForwarding) == (description.operation <= BackendOperation::Responses) && description.model_types == k_all_operation_model_types, "Moonshine transcription is implemented; chat streams are legacy forwarding");
    }
    check(server.operation_bindings()->descriptions().size() == 5, "Moonshine binds five cells");
    auto* realtime = operation_adapter<IStreamingTranscriptionServer>(server, BackendOperation::StreamingTranscription, OperationDelivery::Realtime, ModelType::TRANSCRIPTION);
    check(realtime && realtime->get_streaming_address() == "tcp://127.0.0.1:" + std::to_string(upstream.port + 2), "realtime adapter returns the TCP address");
    auto* adapter = operation_adapter<ITranscriptionServer>(server, BackendOperation::Transcription, OperationDelivery::Buffered, ModelType::TRANSCRIPTION);
    const std::string audio("RIFF\0\xff\r\nWAVE", 12);
    {
        LogCapture capture("MoonshineServer");
        Arm verbose(upstream.port, "debug");
        verbose.load(model());
        auto* logged = operation_adapter<ITranscriptionServer>(*verbose.server, BackendOperation::Transcription, OperationDelivery::Buffered, ModelType::TRANSCRIPTION);
        capture.records.clear();
        check(logged->audio_transcriptions({{"file_data", audio}, {"filename", "dir/clip.mp3"}}) == json{{"text", "fixture transcription"}}, "Moonshine parses the JSON transcription");
        check(capture.records == std::vector<std::string>{"Debug Audio data size: 12 bytes", "Debug Sending multipart request to http://127.0.0.1:" + std::to_string(upstream.port) + "/inference (direct data)", "Debug Response status: 200"}, "Moonshine request diagnostics");
    }
    check(upstream.last_target() == "/inference" && upstream.field("file") == audio && upstream.file("filename") == "clip.mp3" && upstream.file("type") == "audio/mpeg" &&
          upstream.field("response_format") == "json" && upstream.field("language") == "<absent>" && upstream.field("prompt") == "<absent>", "Moonshine multipart fields, basename, MIME and default format");
    adapter->audio_transcriptions({{"file_data", audio}, {"response_format", "verbose_json"}, {"language", "pl"}, {"prompt", "Dzień"}});
    check(upstream.file("filename") == "audio.wav" && upstream.file("type") == "audio/wav" && upstream.field("response_format") == "verbose_json" && upstream.field("language") == "pl" && upstream.field("prompt") == u8"Dzień", "Moonshine explicit fields and default filename");
    upstream.respond(200, "plain text");
    check(adapter->audio_transcriptions({{"file_data", audio}}) == json{{"text", "plain text"}}, "Moonshine wraps a non-JSON body as text");
    const std::vector<std::tuple<int, std::string, json>> errors = {
        {400, R"({"error":"bad input"})", {{"error", {{"message", "Transcription failed: bad input"}, {"type", "invalid_request_error"}, {"status_code", 400}}}}},
        {500, R"({"error":{"message":"Not a valid RIFF file"}})", {{"error", {{"message", "Transcription failed: Not a valid RIFF file"}, {"type", "invalid_request_error"}, {"status_code", 400}}}}},
        {500, "crash", {{"error", {{"message", "Transcription failed: crash"}, {"type", "audio_processing_error"}, {"status_code", 500}}}}},
        {503, R"({"error":{"code":1}})", {{"error", {{"message", R"(Transcription failed: {"error":{"code":1}})"}, {"type", "audio_processing_error"}, {"status_code", 503}}}}},
    };
    for (const auto& [code, body, expected] : errors) {
        upstream.respond(code, body);
        check(adapter->audio_transcriptions({{"file_data", audio}}) == expected, "Moonshine error envelope for HTTP " + std::to_string(code));
    }
    upstream.respond(200, R"({"text":"fixture transcription"})");
    const auto before = upstream.requests();
    const std::vector<std::pair<json, std::string>> invalid = {
        {json::object(), "Transcription failed: Missing 'file_data' in request"},
        {{{"file_data", ""}}, "Transcription failed: Empty audio data"},
        {{{"file_data", 5}}, "Transcription failed: [json.exception.type_error.302] type must be string, but is number"},
        {{{"file_data", audio}, {"filename", 5}}, "Transcription failed: [json.exception.type_error.302] type must be string, but is number"},
        {{{"file_data", 5}, {"filename", 5}}, "Transcription failed: [json.exception.type_error.302] type must be string, but is number"},
    };
    for (const auto& [request, message] : invalid) {
        check(adapter->audio_transcriptions(request) == json{{"error", {{"message", message}, {"type", "audio_processing_error"}, {"status_code", 500}}}}, "Moonshine invalid input: " + message);
    }
    check(upstream.requests() == before, "invalid Moonshine input never reaches the backend");
    check(adapter->audio_transcriptions({{"file_data", audio}, {"language", 5}})["error"]["message"] == "Transcription failed: [json.exception.type_error.302] type must be string, but is number" && upstream.requests() == before, "non-string language fails before transport");
    for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
        const std::string label = operation == BackendOperation::ChatCompletion ? "chat completion" : operation == BackendOperation::Completion ? "text completion" : "responses";
        check(server.dispatch_completion_operation(operation, json::object()) == json{{"error", {{"message", "Moonshine models do not support " + label + ". Use audio transcription endpoints instead."}, {"type", "unsupported_operation"}, {"code", "model_not_applicable"}}}}, "exact Moonshine rejection");
    }
    for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        Capture output;
        server.forward_streaming_request(endpoint, "{ \"stream\" : true }", output.sink);
        check(upstream.last_target() == endpoint && upstream.last_body() == "{ \"stream\" : true }" && output.bytes == k_sse, "Moonshine legacy streaming forwards original bytes");
    }
}

void check_registration() {
    const auto entry = moonshine::configured_entry();
    descriptor(entry->descriptor, moonshine::descriptor);
    const auto& d = entry->descriptor;
    check(d.recipe == "moonshine" && d.binary == "moonshine-server" && d.default_device == DEVICE_CPU && d.supported_modes == std::vector<std::string>{"transcription"} &&
          d.default_capabilities == std::vector<std::string>{"realtime-transcription"} && d.arg_variants == std::vector<std::string>{"cpu"} && d.takes_args, "independent Moonshine descriptor identities");
    check(d.options.size() == 1 && d.options[0].name == "moonshine_args" && d.options[0].cli_flag == "--moonshine-args" && d.support.size() == 3 && d.support[1].devices == DeviceConstraints{{"cpu", {"x86_64", "arm64"}}}, "independent Moonshine option and support rows");
    check(entry->compatibility == TrustedCompatibility::LegacyNonChatSse && entry->launch_compatibility.device_on_backend_selection == DEVICE_CPU && !entry->launch_compatibility.posix_executable_loader_path && entry->launch_compatibility.load_log, "Moonshine carries the legacy SSE, CPU device and load-log attachments");
    check(moonshine::capabilities() == (CAP_TRANSCRIPTION | CAP_STREAMING_TRANSCRIPTION), "Moonshine capability export");
    const auto* spec = moonshine::spec();
    const std::vector<std::tuple<std::string, std::string, std::string>> assets = {
        {"windows", "x86_64", "moonshine-server-v9-windows-x64.zip"}, {"windows", "aarch64", "moonshine-server-v9-windows-x64.zip"},
        {"macos", "aarch64", "moonshine-server-v9-macos-arm64.tar.gz"}, {"macos", "x86_64", "moonshine-server-v9-macos-arm64.tar.gz"},
        {"linux", "aarch64", "moonshine-server-v9-linux-arm64.tar.gz"}, {"linux", "x86_64", "moonshine-server-v9-linux-x64.tar.gz"}, {"linux", "", "moonshine-server-v9-linux-x64.tar.gz"},
    };
    for (const auto& [os, arch, filename] : assets) for (const char* backend : {"cpu", "rocm", ""}) {
        InstallContext context{*spec, spec->definition, "moonshine", "moonshine", backend, "", "v9", "v9", {os, arch, ""}, InstallPhase::Asset};
        const auto params = spec->install_params_fn(context);
        check(params.repo == "lemonade-sdk/moonshine-server-rocm" && params.filename == filename, "Moonshine asset " + os + "/" + arch + "/" + backend);
        context.phase = InstallPhase::Repository;
        context.pinned_version = "pin";
        check(spec->install_params_fn(context).filename.find("-pin-") != std::string::npos, "Moonshine repository phase uses the pinned version");
    }
    std::vector<std::string> probes;
    install_platform(*spec, "cpu", {[&] { probes.push_back("os"); return std::string("linux"); }, [&] { probes.push_back("arch"); return std::string("gfx1151"); }});
    check(probes == std::vector<std::string>{"os"}, "Moonshine installation never probes the ROCm architecture");
    const auto* ops = moonshine::ops();
    const std::vector<std::string> files = {"medium-streaming-en/quantized/model.onnx", "Medium-Streaming-EN/Quantized/tokens.json", "medium-streaming-en/quantizedX/other", "README.md"};
    check(ops->select_checkpoint_files("medium-streaming-en/quantized", files) == std::optional<std::vector<std::string>>{{"medium-streaming-en/quantized/model.onnx", "Medium-Streaming-EN/Quantized/tokens.json"}}, "Moonshine selects the variant directory case-insensitively");
    check(ops->select_checkpoint_files("medium-streaming-en/quantized/", files) == ops->select_checkpoint_files("medium-streaming-en/quantized", files), "a trailing slash names the same directory");
    check(ops->select_checkpoint_files("", files) == std::optional<std::vector<std::string>>{files}, "an empty variant selects every file");
    std::string message;
    try { ops->select_checkpoint_files("absent", files); } catch (const std::runtime_error& error) { message = error.what(); }
    check(message == "No Moonshine model files found in folder: absent", "Moonshine reports an empty variant directory");
    CheckpointResolveContext context;
    context.model_cache_path = "/cache/models--org--moonshine";
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == default_backend_ops()->resolve_checkpoint_path(ModelInfo{}, context) && ops->find_imported_checkpoint("/import").empty(), "Moonshine keeps default checkpoint resolution and import lookup");
}
}

int main() {
    Upstream upstream;
    check_registration();
    check_launch(upstream);
    check_failure_states(upstream);
    check_logging(upstream);
    check_transcription(upstream);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
