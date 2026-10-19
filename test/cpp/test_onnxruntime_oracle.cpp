#include "lemon/backends/onnxruntime/onnxruntime_server.h"
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
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Post("/classify", [this](const auto& request, auto& response) { record(request); response.set_content(R"({"labels":{"safe":0.9}})", "application/json"); });
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

struct Tree {
    fs::path root = fs::temp_directory_path() / ("onnx-oracle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { fs::create_directories(root); }
    ~Tree() { fs::remove_all(root); }
    fs::path complete(const std::string& relative, bool config = true) const {
        const auto dir = relative.empty() ? root : root / relative;
        fs::create_directories(dir);
        std::ofstream(dir / "model.onnx") << "onnx";
        std::ofstream(dir / "tokenizer.json") << "{}";
        if (config) std::ofstream(dir / "config.json") << "{}";
        return dir;
    }
    std::string script(const std::string& name, const std::string& body) const {
        fs::create_directories(root / "bin");
        const auto path = root / "bin" / name;
        std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
        fs::permissions(path, fs::perms::owner_all);
        return path.string();
    }
};

struct Arm {
    Fixture fixture;
    std::vector<int> probes;
    std::unique_ptr<WrappedServer> server;
    struct HostServices : EffectLaunchServices {
        explicit HostServices(BackendEffects effects) : EffectLaunchServices(std::move(effects)) {}
        PlatformInfo platform() const override { return {"linux", "x86_64", ""}; }
    };
    Arm(int port, const Tree& tree, const std::string& level = "error") : fixture(port) {
        fixture.executable = "/bundle/onnx/ort-server";
        fixture.paths = {tree.root.string()};
        auto effects = fixture.effects();
        effects.find_free_port = [this](int start) { fixture.events.push_back("probe:" + std::to_string(start)); return fixture.port; };
        server = std::make_unique<ConfiguredServer>(onnxruntime::configured_entry(), BackendContext{level}, ConfiguredServerServices{std::make_shared<HostServices>(effects), effects.make_process});
    }
    std::string load(const Tree& tree, const RecipeOptions& options = RecipeOptions("onnxruntime", json::object())) {
        ModelInfo model;
        model.checkpoints["main"] = "org/guard";
        model.resolved_paths["main"] = tree.root.string();
        server->set_model_metadata("fixture", "org/guard", ModelType::CLASSIFICATION, DEVICE_NPU, options);
        try { server->load("fixture", model, options, false); }
        catch (const std::exception& error) { return error.what(); }
        server->set_state(ModelState::READY);
        return "";
    }
};

void check_launch(Upstream& upstream) {
    {
        Tree tree;
        const auto dir = tree.complete("snapshots/abc/onnx");
        Arm arm(upstream.port, tree, "debug");
        LogCapture capture("OnnxRuntimeServer");
        check(arm.load(tree).empty(), "ONNX Runtime loads through the configured server");
        check(arm.fixture.command.program == "/bundle/onnx/ort-server" && arm.fixture.command.args == std::vector<std::string>{"--model-path", dir.string(), "--port", std::to_string(upstream.port)}, "ONNX Runtime argv names the servable directory");
        check(arm.fixture.events == std::vector<std::string>{"install:onnxruntime:cpu", "exists:" + tree.root.string(), "managed:onnxruntime:cpu", "probe:8001", "process", "start"}, "ONNX Runtime install, model check, lookup, single probe and start order");
        check(arm.fixture.command.env.empty() && arm.fixture.command.ready_endpoint == "/health" && arm.fixture.cwd.empty() && !arm.fixture.output.filter_health_logs && arm.server->get_device_type() == DEVICE_CPU, "ONNX Runtime environment, readiness, cwd, output and CPU device");
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture",
            "Info Per-model settings: onnxruntime_args=(none), merge_args=true, auto_evict=(none), evict_idle_timeout=300, downsize_idle_timeout=60, evict_weight_factor=1.000000, pinned=false, auto_update=(none)",
            "Info Using model: " + dir.string(), "Info Using executable: /bundle/onnx/ort-server"}, "ONNX Runtime load diagnostics");
    }
    for (const char* level : {"info", "debug", "trace", "warning", "error"}) {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree, level);
        arm.load(tree);
        const std::string value = level;
        check(arm.fixture.output.inherit == (value == "info" || value == "debug" || value == "trace"), "ONNX Runtime inherits child output at info and debug: " + value);
    }
    {
        Tree tree;
        tree.complete("m", false);
        Arm arm(upstream.port, tree, "debug");
        LogCapture capture("OnnxRuntimeServer");
        check(arm.load(tree) == "No servable model directory under '" + tree.root.string() + "': need model.onnx + tokenizer.json + config.json (manifest.json is optional and overrides the output contract)" &&
              arm.fixture.events.size() == 2 && capture.records.size() == 2, "incomplete layout fails before lookup and the model line");
    }
    {
        Tree tree;
        const auto b = tree.complete("b");
        const auto a = tree.complete("a/nested");
        Arm arm(upstream.port, tree);
        check(arm.load(tree) == "Ambiguous model layout under '" + tree.root.string() + "': 2 complete model directories found — keep exactly one:\n  " + a.string() + "\n  " + b.string(), "two complete directories are listed in sorted order");
        Tree nested;
        nested.complete("");
        const auto inner = nested.complete("inner");
        Arm second(upstream.port, nested);
        check(second.load(nested) == "Ambiguous model layout under '" + nested.root.string() + "': 2 complete model directories found — keep exactly one:\n  " + nested.root.string() + "\n  " + inner.string(), "a complete root with a nested model is ambiguous");
    }
    {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree);
        arm.fixture.paths.clear();
        check(arm.load(tree) == "Model directory not found for checkpoint: org/guard" && arm.fixture.events.size() == 2 && arm.server->get_device_type() == DEVICE_CPU, "missing model fails after install with the CPU device set");
        Arm zero(upstream.port, tree);
        zero.fixture.port = 0;
        check(zero.load(tree) == "Failed to find an available port for ort-server" && zero.fixture.events.back() == "probe:8001", "zero port error after one probe");
    }
    for (const char* flag : {"--model-path", "--port"}) for (const char* suffix : {" x", "=x"}) {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree);
        const auto message = arm.load(tree, RecipeOptions("onnxruntime", {{"onnxruntime_args", std::string(flag) + suffix}}));
        check(message.rfind("Invalid custom ort-server arguments:\n", 0) == 0 && arm.fixture.events.back() == "probe:8001", std::string("ONNX Runtime rejects a managed flag after the port: ") + flag + suffix);
    }
    {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree, "debug");
        LogCapture capture("OnnxRuntimeServer");
        check(arm.load(tree, RecipeOptions("onnxruntime", {{"onnxruntime_args", "--threads 2"}})).empty() && arm.fixture.command.args.size() == 6 && arm.fixture.command.args[4] == "--threads" && capture.records.size() == 4, "ONNX Runtime appends custom arguments without a log line");
    }
}

void check_failure_states(Upstream& upstream) {
    const RecipeOptions invalid("onnxruntime", {{"onnxruntime_args", "--port 1"}});
    const std::string rejected = "Invalid custom ort-server arguments:\n";
    Tree tree;
    tree.complete("m");
    {
        Arm arm(upstream.port, tree);
        check(arm.load(tree, invalid).rfind(rejected, 0) == 0 && arm.fixture.events.back() == "probe:8001" && arm.server->get_backend_port() == 0, "a fresh ONNX Runtime object that fails after probing publishes no HTTP port");
    }
    {
        Arm arm(upstream.port, tree);
        check(arm.load(tree).empty() && arm.server->get_backend_port() == upstream.port, "ONNX Runtime loads before the failed reloads");
        arm.fixture.port = upstream.port + 1;
        check(arm.load(tree, invalid).rfind(rejected, 0) == 0 && arm.server->get_backend_port() == upstream.port, "a failed ONNX Runtime preparation keeps the previous HTTP port instead of the newly probed one");
        arm.fixture.port = 0;
        check(arm.load(tree) == "Failed to find an available port for ort-server" && arm.server->get_backend_port() == upstream.port, "a failed ONNX Runtime probe keeps the previous HTTP port");
    }
#ifndef _WIN32
    {
        Arm arm(upstream.port, tree);
        arm.fixture.executable = tree.script("ort-server", "exit 3");
        arm.fixture.child_running = false;
        check(arm.load(tree) == "ort-server failed to start: exited with code 3; installed: ort-server" && arm.server->get_backend_port() == 0, "an ONNX Runtime readiness failure resets the HTTP port");
    }
#endif
}

void check_startup_error(Upstream& upstream) {
#ifndef _WIN32
    const std::vector<std::pair<std::string, std::string>> scripts = {
        {"echo; echo 'ort-server: unsupported architecture'; echo '  second  '; exit 3", "ort-server failed to start: ort-server: unsupported architecture |   second"},
        {"exit 7", "ort-server failed to start: exited with code 7; installed: ort-server"},
        {"printf '%0300d\\n' 1; printf '%0300d\\n' 2; printf '%0300d\\n' 3", "ort-server failed to start: " + std::string(299, '0') + "1 | " + std::string(97, '0') + "..."},
    };
    for (const auto& [body, expected] : scripts) {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree);
        arm.fixture.executable = tree.script("ort-server", body);
        arm.fixture.child_running = false;
        check(arm.load(tree) == expected && arm.fixture.stops == 1, "ONNX Runtime reruns the refused child to explain the failure: " + expected.substr(0, 60));
    }
    {
        Tree tree;
        tree.complete("m");
        Arm arm(upstream.port, tree);
        arm.fixture.executable = tree.script("ort-server", "touch \"$0.ran\"; exit 3");
        arm.fixture.child_running = false;
        std::atomic<bool> cancel{true};
        arm.server->set_load_cancel_flag(&cancel);
        check(arm.load(tree) == "ort-server failed to start" && !fs::exists(arm.fixture.executable + ".ran"), "a cancelled load is not rerun");
    }
#else
    (void)upstream;
#endif
}

void check_operations(Upstream& upstream) {
    Tree tree;
    tree.complete("m");
    Arm arm(upstream.port, tree);
    arm.load(tree);
    auto& server = *arm.server;
    for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) for (const auto mode : k_operation_model_types) {
        const bool expected = (op <= 2 && delivery == 1) || (op == 5 && delivery == 0);
        check(server.supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected, "ONNX Runtime operation matrix " + std::to_string(op) + "/" + std::to_string(delivery));
    }
    check(server.operation_bindings()->descriptions().size() == 4, "ONNX Runtime binds four cells");
    auto* adapter = operation_adapter<IClassificationServer>(server, BackendOperation::Classification, OperationDelivery::Buffered, ModelType::CLASSIFICATION);
    const std::vector<std::pair<json, json>> requests = {
        {{{"text", "hello"}, {"input", "ignored"}}, {{"text", "hello"}}},
        {{{"text", 5}, {"input", "fallback"}, {"top_k", 3}}, {{"text", "fallback"}, {"top_k", 3}}},
        {{{"input", "only"}, {"top_k", nullptr}}, {{"text", "only"}, {"top_k", nullptr}}},
    };
    for (const auto& [request, expected] : requests) {
        check(adapter->classify(request) == json{{"labels", {{"safe", 0.9}}}} && upstream.last_target() == "/classify" && json::parse(upstream.last_body()) == expected, "ONNX Runtime classify body " + expected.dump());
    }
    const auto before = upstream.requests();
    for (const auto& request : {json::object(), json{{"text", 1}, {"input", false}}}) {
        check(adapter->classify(request) == json{{"error", {{"message", "Missing 'input' (or 'text') string in classify request"}, {"type", "invalid_request_error"}, {"status_code", 400}}}}, "ONNX Runtime rejects a classify request without text");
    }
    check(upstream.requests() == before, "invalid classify requests never reach the backend");
    for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
        const std::string label = operation == BackendOperation::ChatCompletion ? "chat completion" : operation == BackendOperation::Completion ? "text completion" : "responses";
        check(server.dispatch_completion_operation(operation, json::object()) == json{{"error", {{"message", "ort-server does not support " + label + ". Use the appropriate endpoint for this model type instead."}, {"type", "unsupported_operation"}, {"code", "model_not_applicable"}}}}, "ONNX Runtime keeps the generic rejection");
    }
    for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        Capture output;
        server.forward_streaming_request(endpoint, "{ \"stream\" : true }", output.sink);
        check(upstream.last_target() == endpoint && upstream.last_body() == "{ \"stream\" : true }" && output.bytes == k_sse, "ONNX Runtime legacy streaming forwards original bytes");
    }
}

void check_registration() {
    const auto entry = onnxruntime::configured_entry();
    descriptor(entry->descriptor, registered_descriptor("onnxruntime"));
    const auto& d = entry->descriptor;
    check(d.recipe == "onnxruntime" && d.binary == "ort-server" && d.default_device == DEVICE_CPU && d.supported_modes == std::vector<std::string>{"classification"} && d.support.size() == 3 && d.support[2].device_summary == "arm64 CPU", "independent ONNX Runtime descriptor identities");
    check(entry->compatibility == TrustedCompatibility::LegacyNonChatSse && entry->launch_compatibility.device_on_backend_selection == DEVICE_CPU && entry->launch_compatibility.load_log, "ONNX Runtime carries the legacy SSE, CPU device and load-log attachments");
    check(onnxruntime::capabilities() == CAP_CLASSIFICATION, "ONNX Runtime capability export");
    const auto* spec = onnxruntime::spec();
    const std::vector<std::tuple<std::string, std::string, std::string>> assets = {
        {"windows", "x86_64", "ort-server-0.3.7-windows-x64.zip"}, {"windows", "", "ort-server-0.3.7-windows-x64.zip"},
        {"macos", "aarch64", "ort-server-0.3.7-macos-arm64.tar.gz"}, {"macos", "x86_64", "ort-server-0.3.7-macos-arm64.tar.gz"},
        {"linux", "aarch64", "ort-server-0.3.7-linux-arm64.tar.gz"}, {"linux", "x86_64", "ort-server-0.3.7-linux-x64.tar.gz"}, {"linux", "", "ort-server-0.3.7-linux-x64.tar.gz"},
    };
    for (const auto& [os, arch, filename] : assets) for (const char* backend : {"cpu", "rocm"}) {
        const InstallContext context{*spec, spec->definition, "onnxruntime", "onnxruntime", backend, "", "0.3.6", "0.3.7", {os, arch, ""}, InstallPhase::Asset};
        const auto params = spec->install_params_fn(context);
        check(params.repo == "lemonade-sdk/ort-server" && params.filename == filename, "ONNX Runtime asset " + os + "/" + arch);
    }
    std::string message;
    try { spec->install_params_fn({*spec, spec->definition, "onnxruntime", "onnxruntime", "cpu", "", "0.3.7", "0.3.7", {"windows", "aarch64", ""}, InstallPhase::Asset}); }
    catch (const std::runtime_error& error) { message = error.what(); }
    check(message == "The onnxruntime backend has no Windows-on-ARM64 build of ort-server yet", "ONNX Runtime rejects Windows on ARM64");
    const auto* ops = onnxruntime::ops();
    Tree tree;
    fs::create_directories(tree.root / "refs");
    std::ofstream(tree.root / "refs/main") << "new";
    const auto active = tree.complete("snapshots/new/onnx");
    tree.complete("snapshots/old/onnx");
    CheckpointResolveContext context;
    context.model_cache_path = tree.root.string();
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == active.string(), "ONNX Runtime scopes resolution to the active snapshot");
    tree.complete("snapshots/new/second");
    check(ops->resolve_checkpoint_path(ModelInfo{}, context) == (tree.root / "snapshots/new").string(), "an ambiguous active snapshot resolves to the snapshot");
    fs::remove(tree.root / "refs/main");
    {
        LogCapture capture("OnnxRuntimeServer");
        check(ops->resolve_checkpoint_path(ModelInfo{}, context) == tree.root.string() && capture.records == std::vector<std::string>{"Warn 3 complete model directories under " + tree.root.string() + "; refusing to pick one"}, "an ambiguous import resolves to the cache root with a warning");
    }
    check(ops->find_imported_checkpoint((tree.root / "absent").string()).empty() && ops->find_imported_checkpoint((tree.root / "snapshots/old").string()) == (tree.root / "snapshots/old/onnx").string(), "ONNX Runtime import lookup");
    check(!ops->select_checkpoint_files("", {"model.onnx"}) && ops->validate_checkpoint_file("x").empty(), "ONNX Runtime keeps default file selection and validation");
}
}

int main() {
    Upstream upstream;
    check_registration();
    check_launch(upstream);
    check_failure_states(upstream);
    check_startup_error(upstream);
    check_operations(upstream);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
