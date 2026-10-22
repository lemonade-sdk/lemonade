#include "lemon/backends/kokoro/kokoro_server.h"
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
namespace fs = std::filesystem;
const std::string k_sse = "data: {\"fixture\":true}\n\ndata: [DONE]\n\n";

class Upstream {
public:
    Upstream() {
        http.Get("/", [](const auto&, auto& response) { response.set_content("ok", "text/plain"); });
        http.Post("/v1/audio/speech", [this](const auto& request, auto& response) { record(request); response.set_content(std::string("PCM\0\x01", 5), "audio/pcm"); });
        for (const char* path : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
            http.Post(path, [this](const auto& request, auto& response) { record(request); response.set_content(k_sse, "text/event-stream"); });
        }
        port = http.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("Fake loopback bind failed");
        thread = std::thread([this] { http.listen_after_bind(); });
        http.wait_until_ready();
    }
    ~Upstream() { http.stop(); thread.join(); }
    void record(const httplib::Request& request) { std::lock_guard<std::mutex> lock(mutex); target = request.target; body = request.body; ++count; }
    std::string last_target() { std::lock_guard<std::mutex> lock(mutex); return target; }
    std::string last_body() { std::lock_guard<std::mutex> lock(mutex); return body; }
    size_t requests() { std::lock_guard<std::mutex> lock(mutex); return count; }
    httplib::Server http;
    int port = 0;
private:
    std::thread thread;
    std::mutex mutex;
    std::string target, body;
    size_t count = 0;
};

struct Model {
    fs::path root = fs::temp_directory_path() / ("kokoro-oracle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path index = root / "voice model" / "index.json";
    explicit Model(const std::string& text = R"({"model":"kokoro-v1.0.onnx","voices":"voices-v1.0.bin"})") {
        fs::create_directories(index.parent_path());
        std::ofstream(index) << text;
    }
    ~Model() { fs::remove_all(root); }
    ModelInfo info() const { ModelInfo model; model.checkpoints["main"] = "fixture/repo"; model.resolved_paths["main"] = index.string(); return model; }
};

struct Arm {
    Fixture fixture;
    std::shared_ptr<Services> services;
    std::unique_ptr<WrappedServer> server;
    Arm(int port, const Model& model, const std::string& level = "error", const std::string& os = "linux") : fixture(port) {
        fixture.executable = "/bundle/kokoro/koko";
        fixture.paths = {model.index.string()};
        services = std::make_shared<Services>(fixture);
        services->host = {os, "x86_64", ""};
        server = std::make_unique<ConfiguredServer>(kokoro::configured_entry(), BackendContext{level}, ConfiguredServerServices{services, fixture.effects().make_process});
    }
    std::string load(const ModelInfo& model) {
        server->set_model_metadata("fixture", "fixture/repo", ModelType::TTS, DEVICE_NPU, RecipeOptions("kokoro", json::object()));
        try { server->load("fixture", model, RecipeOptions("kokoro", json::object()), false); }
        catch (const std::exception& error) { return error.what(); }
        server->set_state(ModelState::READY);
        return "";
    }
};

void check_launch(Upstream& upstream) {
    for (const auto& inherited : {std::optional<std::string>("old"), std::optional<std::string>(""), std::optional<std::string>()}) {
        Model model;
        Arm arm(upstream.port, model, "error");
        arm.fixture.inherited = inherited;
        check(arm.load(model.info()).empty(), "Kokoro loads through the configured server");
        const auto dir = model.index.parent_path();
        check(arm.fixture.command.program == "/bundle/kokoro/koko" && arm.fixture.command.args == std::vector<std::string>{"-m", (dir / "kokoro-v1.0.onnx").string(), "-d", (dir / "voices-v1.0.bin").string(), "openai", "--ip", "127.0.0.1", "--port", std::to_string(upstream.port)}, "Kokoro argv joins the index entries to the model directory");
        const std::string loader = inherited && !inherited->empty() ? "/bundle/kokoro:" + *inherited : "/bundle/kokoro";
        check(arm.fixture.command.env == std::vector<std::pair<std::string, std::string>>{{"ESPEAK_DATA_PATH", "/bundle/kokoro/espeak-ng-data"}, {"LD_LIBRARY_PATH", loader}}, "Kokoro environment order and POSIX loader without a trailing separator");
        check(arm.fixture.events == std::vector<std::string>{"install:kokoro:cpu", "exists:" + model.index.string(), "managed:kokoro:cpu", "port", "posix-loader", "process", "start"}, "Kokoro install, model check, lookup, port and loader order");
        check(arm.fixture.command.ready_endpoint == "/" && arm.fixture.cwd.empty() && !arm.fixture.output.filter_health_logs && arm.server->get_device_type() == DEVICE_NPU, "Kokoro readiness, cwd, output filter and untouched device");
    }
    for (const char* level : {"debug", "trace", "info", "error"}) {
        Model model;
        Arm arm(upstream.port, model, level);
        arm.load(model.info());
        const std::string value = level;
        check(arm.fixture.output.inherit == (value == "debug" || value == "trace"), "Kokoro inherits child output at debug only: " + value);
    }
    {
        Model model;
        Arm arm(upstream.port, model, "error", "windows");
        check(arm.load(model.info()).empty() && arm.fixture.command.env == std::vector<std::pair<std::string, std::string>>{{"ESPEAK_DATA_PATH", "/bundle/kokoro/espeak-ng-data"}}, "Windows host sets no loader path");
        check(arm.fixture.events.front() == "install:kokoro:cpu" && std::find(arm.fixture.events.begin(), arm.fixture.events.end(), "posix-loader") == arm.fixture.events.end(), "Windows host installs cpu and never reads the POSIX loader");
    }
    {
        Model model;
        Arm arm(upstream.port, model, "error", "macos");
        check(arm.load(model.info()).empty() && arm.fixture.events[0] == "install:kokoro:metal" && arm.fixture.events[2] == "managed:kokoro:metal", "macOS host selects the metal backend");
    }
    {
        Model model;
        Arm arm(upstream.port, model);
        arm.fixture.paths.clear();
        LogCapture capture("KokoroServer");
        check(arm.load(model.info()) == "Model file not found for checkpoint: fixture/repo" && arm.fixture.events.size() == 2 && capture.records == std::vector<std::string>{"Info Loading model: fixture"}, "missing model fails after install and before reading the index");
        auto empty = model.info();
        empty.resolved_paths["main"] = "";
        Arm second(upstream.port, model);
        check(second.load(empty) == "Model file not found for checkpoint: fixture/repo" && second.fixture.events == std::vector<std::string>{"install:kokoro:cpu"}, "empty resolved path fails without an existence probe");
    }
    {
        Model model;
        Arm arm(upstream.port, model);
        arm.fixture.port = 0;
        check(arm.load(model.info()) == "Failed to find an available port", "zero port error");
        Arm exited(upstream.port, model);
        exited.fixture.child_running = false;
        check(exited.load(model.info()) == "kokoro-server failed to start" && exited.fixture.stops == 1, "Kokoro keeps the kokoro-server process identity");
    }
}

void check_index(Upstream& upstream) {
    {
        Model model;
        Arm arm(upstream.port, model, "debug");
        LogCapture capture("KokoroServer");
        arm.load(model.info());
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture", "Info Reading \"index.json\"", "Info Using model: \"kokoro-v1.0.onnx\"",
            "Info Starting server on port " + std::to_string(upstream.port), "Info Setting LD_LIBRARY_PATH=/bundle/kokoro:old"}, "Kokoro keeps its load diagnostics in order");
    }
    {
        Model model;
        fs::remove(model.index);
        Arm arm(upstream.port, model);
        LogCapture capture("KokoroServer");
        check(arm.load(model.info()) == "Warning: Could not load index.json: Failed to open file: " + model.index.string() && arm.fixture.events.size() == 2 &&
              capture.records == std::vector<std::string>{"Info Loading model: fixture", "Info Reading \"index.json\""}, "unreadable index fails after the reading line and before lookup");
    }
    {
        Model model("{not json");
        Arm arm(upstream.port, model);
        const auto message = arm.load(model.info());
        check(message.rfind("Warning: Could not load index.json: Failed to parse JSON from file " + model.index.string() + ": [json.exception.parse_error", 0) == 0 && arm.fixture.events.size() == 2, "malformed index keeps the wrapped parse error");
    }
    {
        Model model(R"({"model":"kokoro-v1.0.onnx"})");
        Arm arm(upstream.port, model, "debug");
        LogCapture capture("KokoroServer");
        check(arm.load(model.info()) == "[json.exception.type_error.302] type must be string, but is null" &&
              arm.fixture.events == std::vector<std::string>{"install:kokoro:cpu", "exists:" + model.index.string(), "managed:kokoro:cpu", "port", "posix-loader"}, "missing voices entry fails when argv is built, after the port and loader");
        check(capture.records.size() == 5 && capture.records[3] == "Info Starting server on port " + std::to_string(upstream.port) && capture.records[4] == "Info Setting LD_LIBRARY_PATH=/bundle/kokoro:old", "missing voices entry still logs the startup and loader lines first");
    }
    {
        Model model(R"({"voices":"voices.bin","model":5})");
        Arm arm(upstream.port, model, "debug");
        LogCapture capture("KokoroServer");
        check(arm.load(model.info()) == "[json.exception.type_error.302] type must be string, but is number" && capture.records[2] == "Info Using model: 5", "non-string model entry logs its JSON and fails at argv");
    }
    {
        Model model(R"({"voices":"voices.bin"})");
        Arm arm(upstream.port, model, "debug");
        LogCapture capture("KokoroServer");
        check(arm.load(model.info()) == "[json.exception.type_error.302] type must be string, but is null" && capture.records[2] == "Info Using model: null", "absent model entry logs null and fails at argv");
    }
    {
        Model model(R"({"model":"/absolute/net.onnx","voices":"voices.bin"})");
        Arm arm(upstream.port, model);
        check(arm.load(model.info()).empty() && arm.fixture.command.args[1] == "/absolute/net.onnx", "absolute index entries replace the model directory");
    }
}

void check_operations(Upstream& upstream) {
    Model model;
    Arm arm(upstream.port, model);
    arm.load(model.info());
    auto& server = *arm.server;
    const auto tts = operation_model_type_mask(ModelType::TTS);
    for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) for (const auto mode : k_operation_model_types) {
        const bool expected = (op <= 2 && delivery == 1) || (op == 8 && delivery <= 1 && mode == ModelType::TTS);
        check(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected, "Kokoro operation matrix " + std::to_string(op) + "/" + std::to_string(delivery));
    }
    for (const auto& description : server.operation_bindings()->descriptions()) {
        const bool speech = description.operation == BackendOperation::TextToSpeech;
        check(speech ? description.provenance == OperationProvenance::Implemented && description.model_types == tts
                     : description.provenance == OperationProvenance::LegacyForwarding && description.model_types == k_all_operation_model_types, "Kokoro speech is implemented for tts; chat streams are legacy forwarding");
    }
    check(server.operation_bindings()->descriptions().size() == 5 && server.operation_bindings()->compiled_compatibility(), "Kokoro binds five cells with compiled compatibility");
    auto* buffered = operation_metadata_adapter<ITextToSpeechServer>(&server, BackendOperation::TextToSpeech, OperationDelivery::Buffered);
    auto* streaming = operation_metadata_adapter<ITextToSpeechServer>(&server, BackendOperation::TextToSpeech, OperationDelivery::Streaming);
    check(buffered && buffered->supported_audio_formats() == std::vector<std::string>{"mp3", "wav", "opus", "pcm"}, "Kokoro buffered formats");
    check(streaming && streaming->supported_streaming_audio_formats() == std::vector<std::string>{"pcm"}, "Kokoro streaming remains PCM-only");
    for (const auto* format : {"mp3", "wav", "opus", "pcm", "flac"}) {
        const auto metadata = buffered->audio_format_metadata(format);
        check(metadata.content_type.empty() && metadata.headers.empty(), std::string("Kokoro keeps the server-wide MIME mapping for ") + format);
    }
    const std::vector<std::pair<json, json>> requests = {
        {{{"model", "client"}, {"input", "hi"}, {"response_format", "wav"}}, {{"model", "kokoro"}, {"input", "hi"}, {"response_format", "wav"}}},
        {{{"model", "client"}, {"input", "hi"}, {"stream_format", "audio"}}, {{"model", "kokoro"}, {"input", "hi"}, {"stream_format", "audio"}, {"stream", true}}},
        {{{"input", "hi"}, {"stream_format", nullptr}, {"stream", false}}, {{"model", "kokoro"}, {"input", "hi"}, {"stream_format", nullptr}, {"stream", true}}},
        {{{"input", "hi"}, {"stream", false}}, {{"model", "kokoro"}, {"input", "hi"}, {"stream", false}}},
    };
    for (const auto delivery : {OperationDelivery::Buffered, OperationDelivery::Streaming}) {
        auto* adapter = operation_adapter<ITextToSpeechServer>(server, BackendOperation::TextToSpeech, delivery, ModelType::TTS);
        for (const auto& [request, expected] : requests) {
            Capture audio;
            adapter->audio_speech(request, audio.sink);
            check(upstream.last_target() == "/v1/audio/speech" && json::parse(upstream.last_body()) == expected && upstream.last_body() == expected.dump() &&
                  audio.bytes == std::string("PCM\0\x01", 5) && audio.done == 1, "Kokoro speech wire body and raw bytes");
        }
    }
    const json rejection = {{"error", {{"message", "Kokoro does not support text completion. Use audio speech endpoints instead."}, {"type", "unsupported_operation"}, {"code", "model_not_applicable"}}}};
    const auto before = upstream.requests();
    for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
        check(server.dispatch_completion_operation(operation, json{{"model", "fixture"}}) == rejection, "exact Kokoro buffered rejection");
    }
    check(upstream.requests() == before, "Kokoro buffered rejections never reach the backend");
    for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        const std::string input = "{ \"stream\" : true }";
        Capture output;
        server.forward_streaming_request(endpoint, input, output.sink);
        check(upstream.last_target() == endpoint && upstream.last_body() == input && output.bytes == k_sse, "Kokoro legacy streaming forwards original bytes");
    }
}

void check_registration() {
    const auto entry = kokoro::configured_entry();
    descriptor(entry->descriptor, registered_descriptor("kokoro"));
    const auto& d = entry->descriptor;
#ifdef _WIN32
    const std::string binary = "koko.exe";
#else
    const std::string binary = "koko";
#endif
    check(d.recipe == "kokoro" && d.display_name == "Kokoro" && d.binary == binary && d.default_device == DEVICE_CPU && d.supported_modes == std::vector<std::string>{"tts"} && d.options.empty() && d.bin_variants == std::vector<std::string>{"cpu"} && d.config_defaults() == json{{"cpu_bin", "builtin"}}, "independent Kokoro descriptor identities");
    check(d.support.size() == 2 && d.support[0].backend == "metal" && d.support[0].supported_os == std::set<std::string>{"macos"} && d.support[1].backend == "cpu" && d.support[1].devices == DeviceConstraints{{"cpu", {"x86_64"}}}, "independent Kokoro support rows");
    check(entry->compatibility == TrustedCompatibility::LegacyNonChatSse && !entry->launch_compatibility.device_on_backend_selection && !entry->launch_compatibility.posix_executable_loader_path && entry->launch_compatibility.load_log, "Kokoro carries the legacy SSE and load-log attachments");
    check(kokoro::capabilities() == CAP_TTS, "Kokoro capability export stays TTS");
    const auto* spec = kokoro::spec();
    check(spec->recipe == "kokoro" && spec->binary == binary && spec->pin_recipe == "kokoro" && !spec->supports_split_archive, "Kokoro install identities");
    const std::vector<std::tuple<std::string, std::string, std::string>> assets = {
        {"cpu", "windows", "kokoros-windows-x86_64.tar.gz"}, {"cpu", "linux", "kokoros-linux-x86_64.tar.gz"}, {"cpu", "macos", ""},
        {"metal", "macos", "kokoros-darwin-arm64-metal.tar.gz"}, {"metal", "windows", ""}, {"metal", "linux", ""},
    };
    for (const auto& [backend, os, filename] : assets) for (const auto phase : {InstallPhase::Repository, InstallPhase::Asset}) for (const char* arch : {"x86_64", "aarch64"}) {
        const InstallContext context{*spec, spec->definition, "kokoro", "kokoro", backend, "", "b21", "b22", {os, arch, ""}, phase};
        const auto params = spec->install_params_fn(context);
        check(params.repo == "lemonade-sdk/Kokoros" && params.filename == filename && params.version_override.empty(), "Kokoro asset " + backend + "/" + os);
    }
    for (const char* backend : {"rocm", "", "vulkan"}) {
        const InstallContext context{*spec, spec->definition, "kokoro", "kokoro", backend, "", "b21", "b21", {"linux", "x86_64", ""}, InstallPhase::Asset};
        std::string message;
        try { spec->install_params_fn(context); } catch (const std::runtime_error& error) { message = error.what(); }
        check(message == std::string("[KokoroServer] Unknown kokoros backend: ") + backend, "Kokoro rejects an unknown backend");
    }
    std::vector<std::string> probes;
    install_platform(*spec, "cpu", {[&] { probes.push_back("os"); return std::string("linux"); }, [&] { probes.push_back("arch"); return std::string("gfx1151"); }});
    check(probes == std::vector<std::string>{"os"}, "Kokoro installation never probes the ROCm architecture");
    const auto* ops = kokoro::ops();
    const auto root = fs::temp_directory_path() / ("kokoro-ops-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "snapshots/abc/nested");
    std::ofstream(root / "snapshots/abc/nested/index.json") << "{}";
    CheckpointResolveContext context;
    context.model_cache_path = root.string();
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == (root / "snapshots/abc/nested/index.json").string(), "Kokoro resolves the index.json file inside the model directory");
    fs::remove(root / "snapshots/abc/nested/index.json");
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == root.string(), "Kokoro falls back to the model directory without an index");
    context.model_cache_path = (root / "absent").string();
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == (root / "absent").string(), "Kokoro tolerates an absent cache directory");
    check(ops->find_imported_checkpoint(root.string()).empty() && !ops->select_checkpoint_files("", {"index.json"}), "Kokoro keeps the default import and file selection");
    fs::remove_all(root);
}
}

int main() {
    Upstream upstream;
    check_registration();
    check_launch(upstream);
    check_index(upstream);
    check_operations(upstream);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
