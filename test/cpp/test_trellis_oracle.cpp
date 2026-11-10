#include "configured_fixture.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/trellis/trellis_server.h"
#include "lemon/utils/json_utils.h"

#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace thinksound_oracle;
using namespace lemon::backends;

namespace {
const std::string k_png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0mesh-source", 12);
const std::string k_glb = std::string("glTF\x02\0\0\0", 8) + "mesh";
const std::string k_sse = "data: {\"fixture\":true}\n\ndata: [DONE]\n\n";

class Upstream {
public:
    Upstream() {
        http.Get("/health", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
        http.Post("/generate", [this](const auto& request, auto& response) {
            std::lock_guard<std::mutex> lock(mutex);
            form = request.form; target = request.target; ++count;
            response.status = status;
            response.set_content(reply, "model/gltf-binary");
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
    std::pair<std::string, std::string> image() { std::lock_guard<std::mutex> lock(mutex); return {form.get_file("image").filename, form.get_file("image").content_type}; }
    std::string last_target() { std::lock_guard<std::mutex> lock(mutex); return target; }
    std::string last_body() { std::lock_guard<std::mutex> lock(mutex); return body; }
    size_t requests() { std::lock_guard<std::mutex> lock(mutex); return count; }
    httplib::Server http;
    int port = 0;
private:
    std::thread thread;
    std::mutex mutex;
    httplib::MultipartFormData form;
    std::string target, body, reply = k_glb;
    int status = 200;
    size_t count = 0;
};

std::unique_ptr<WrappedServer> trellis_server(EffectsFixture& fixture, const std::string& level = "error") {
    return std::make_unique<ConfiguredServer>(trellis::configured_entry(), BackendContext{level},
        ConfiguredServerServices{std::make_shared<FixtureLaunchServices>(fixture), fixture.effects().make_process});
}

RecipeOptions options(const json& values = json::object()) {
    RecipeOptions result("trellis", values);
    if (!values.contains("trellis_backend")) result.set_option("trellis_backend", "");
    return result;
}

std::string load(WrappedServer& server, const RecipeOptions& recipe = options(), bool upgrade = false) {
    server.set_model_metadata("fixture", "fixture/repo", ModelType::MESH, DEVICE_GPU, recipe);
    try { server.load("fixture", model(), recipe, upgrade); }
    catch (const std::exception& error) { return error.what(); }
    server.set_state(ModelState::READY);
    return "";
}

const std::string k_model_event = u8"model:/model dir/雪\"trailing\\";

void check_launch(Upstream& upstream) {
    for (const bool upgrade : {false, true}) {
        EffectsFixture fixture(upstream.port);
        auto server = trellis_server(fixture, "debug");
        LogCapture capture("trellis-server");
        check(load(*server, options({{"trellis_args", "--steps 12"}}), upgrade).empty(), "TRELLIS loads through the configured server");
        check(fixture.events == std::vector<std::string>{k_model_event, "supported:trellis", "validate:trellis:vulkan", "external:trellis:vulkan", "install:trellis:vulkan", "managed:trellis:vulkan", "port", "process", "start"},
              "TRELLIS selection, validation, binary resolution and port order ignore the upgrade flag");
        check(fixture.command.program == "/bin dir/ts-server" && fixture.command.args == std::vector<std::string>{"--models", u8"/model dir/雪\"trailing\\", "--host", "127.0.0.1", "--port", std::to_string(upstream.port), "--res", "512", "--steps", "12"}, "TRELLIS argv with custom tail");
        check(fixture.command.env.empty() && fixture.cwd.empty() && fixture.output.inherit && fixture.output.filter_health_logs && fixture.command.ready_endpoint == "/health", "TRELLIS vulkan environment, cwd, filtered debug output and readiness");
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture", "Debug Adding custom arguments: --steps 12", "Info Starting /bin dir/ts-server on port " + std::to_string(upstream.port)}, "TRELLIS keeps its load, custom-argument and startup lines in order");
    }
    for (const char* level : {"debug", "trace", "info", "error"}) {
        EffectsFixture fixture(upstream.port);
        auto server = trellis_server(fixture, level);
        load(*server);
        const std::string value = level;
        check(fixture.output.inherit == (value == "debug" || value == "trace") && fixture.output.filter_health_logs, "TRELLIS inherits output at debug only: " + value);
    }
    for (const auto* backend : {"rocm", "cuda", "rocm-stable", "rocm-nightly"}) {
        EffectsFixture fixture(upstream.port);
        fixture.external = "/external dir/trellis-server";
        auto server = trellis_server(fixture);
        load(*server, options({{"trellis_backend", backend}}));
#ifdef _WIN32
        const std::string key = "PATH";
        const std::string rocm = "/runtime/core:/runtime/libs;/external dir;old";
        const std::string cuda = "/external dir;old";
#else
        const std::string key = "LD_LIBRARY_PATH";
        const std::string rocm = "/runtime/core:/runtime/libs:/external dir:old";
        const std::string cuda = "/external dir:old";
#endif
        std::vector<std::pair<std::string, std::string>> expected;
        if (std::string(backend) == "rocm") expected = {{key, rocm}};
        if (std::string(backend) == "cuda") expected = {{key, cuda}, {"CUDA_VISIBLE_DEVICES", "2,0"}, {"__NV_PRIME_RENDER_OFFLOAD", "1"}};
        check(fixture.command.env == expected, std::string("TRELLIS loader precedes CUDA additions and keeps the exact rocm condition: ") + backend);
        check(std::string(backend) != "cuda" || std::find(fixture.events.begin(), fixture.events.end(), "cuda:trellis-server") != fixture.events.end(), "TRELLIS CUDA additions keep their log tag");
        check(std::find(fixture.events.begin(), fixture.events.end(), "supported:trellis") == fixture.events.end(), "an explicit backend skips the supported query");
        check(fixture.events[1] == std::string("validate:trellis:") + backend && fixture.events[2] == std::string("external:trellis:") + backend &&
              std::find(fixture.events.begin(), fixture.events.end(), std::string("install:trellis:") + backend) == fixture.events.end(), "TRELLIS validates before resolving an external binary and skips install");
    }
    for (const char* flag : {"-m", "--models", "--host", "--port", "--res"}) for (const char* suffix : {" x", "=x"}) {
        EffectsFixture fixture(upstream.port);
        auto server = trellis_server(fixture);
        const auto message = load(*server, options({{"trellis_args", std::string(flag) + suffix}}));
        check(message.rfind("Invalid custom trellis-server arguments:\n", 0) == 0 && fixture.events == std::vector<std::string>{k_model_event}, std::string("TRELLIS validates custom arguments before backend selection and binary lookup: ") + flag + suffix);
    }
    for (int failure = 0; failure < 4; ++failure) {
        EffectsFixture fixture(upstream.port);
        if (failure == 0) fixture.model_exists = false;
        if (failure == 1) fixture.supported.clear();
        if (failure == 2) fixture.port = 0;
        if (failure == 3) fixture.child_running = false;
        auto server = trellis_server(fixture);
        const auto message = load(*server, options({{"trellis_args", failure == 0 ? "--port 1" : ""}}));
        if (failure == 0) check(message == "Model path not found for checkpoint: fixture/repo" && fixture.events.size() == 1, "missing model precedes custom-argument validation");
        if (failure == 1) check(message == "TRELLIS 3D generation not supported by this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected" && fixture.events.size() == 2, "empty backend rejects before validation and install");
        if (failure == 2) check(message == "Failed to find an available port", "zero port error");
        if (failure == 3) check(message == "trellis-server failed to start" && fixture.stops == 1, "TRELLIS keeps the trellis-server process identity");
    }
}

class ScopedEnv {
public:
    ScopedEnv(const char* key, const char* value) : key_(key) {
        if (const char* previous = std::getenv(key)) previous_ = previous;
        set(value);
    }
    ~ScopedEnv() {
        if (previous_) set(previous_->c_str());
#ifdef _WIN32
        else _putenv_s(key_.c_str(), "");
#else
        else unsetenv(key_.c_str());
#endif
    }
private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(key_.c_str(), value);
#else
        setenv(key_.c_str(), value, 1);
#endif
    }
    std::string key_;
    std::optional<std::string> previous_;
};

std::unique_ptr<WrappedServer> trellis_server(EffectsFixture& fixture, BackendEffects effects) {
    return std::make_unique<ConfiguredServer>(trellis::configured_entry(), BackendContext{"debug"},
        ConfiguredServerServices{std::make_shared<EffectLaunchServices>(std::move(effects)), fixture.effects().make_process});
}

void check_accelerator_logging(Upstream& upstream) {
    const ScopedEnv devices("CUDA_VISIBLE_DEVICES", "3");
    const ScopedEnv prime("__NV_PRIME_RENDER_OFFLOAD", "1");
    const auto recipe = [](const char* backend) { return options({{"trellis_backend", backend}, {"trellis_args", "--steps 12"}}); };
    {
        EffectsFixture fixture(upstream.port);
        auto effects = fixture.effects();
        effects.append_cuda = [](auto& env, const std::string& category) { BackendUtils::apply_cuda_env_vars(env, category); };
        auto server = trellis_server(fixture, effects);
        LogCapture capture("trellis-server");
        check(load(*server, recipe("cuda")).empty() && fixture.command.env.size() == 1, "TRELLIS loads the CUDA backend with the production CUDA environment step");
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture", "Info Respecting existing CUDA_VISIBLE_DEVICES=3", "Debug Adding custom arguments: --steps 12",
            "Info Starting /bin dir/ts-server on port " + std::to_string(upstream.port)}, "TRELLIS CUDA environment diagnostics precede the custom-argument line");
    }
    for (const char* backend : {"cuda", "rocm"}) {
        EffectsFixture fixture(upstream.port);
        auto effects = fixture.effects();
        effects.append_cuda = [](auto&, const std::string&) { throw std::runtime_error("accelerator environment witness"); };
        effects.rocm_arch = []() -> std::string { throw std::runtime_error("accelerator environment witness"); };
        auto server = trellis_server(fixture, effects);
        LogCapture capture("trellis-server");
        check(load(*server, recipe(backend)) == "accelerator environment witness" && capture.records == std::vector<std::string>{"Info Loading model: fixture"} &&
              std::find(fixture.events.begin(), fixture.events.end(), "process") == fixture.events.end(), std::string("an accelerator environment failure suppresses the TRELLIS custom-argument line: ") + backend);
    }
}

void check_mesh(Upstream& upstream) {
    EffectsFixture fixture(upstream.port);
    auto server = trellis_server(fixture);
    load(*server);
    for (size_t op = 0; op < k_backend_operation_count; ++op) for (size_t delivery = 0; delivery < k_operation_delivery_count; ++delivery) for (const auto mode : k_operation_model_types) {
        const bool expected = (op <= 2 && delivery == 1) || (op == 14 && delivery == 0);
        check(server->supports_operation(static_cast<BackendOperation>(op), static_cast<OperationDelivery>(delivery), mode) == expected, "TRELLIS operation matrix " + std::to_string(op) + "/" + std::to_string(delivery));
    }
    check(server->operation_bindings()->descriptions().size() == 4, "TRELLIS binds four cells");
    auto* mesh = operation_adapter<IModel3DServer>(*server, BackendOperation::Model3DGeneration, OperationDelivery::Buffered, ModelType::MESH);
    const auto encoded = utils::JsonUtils::base64_encode(k_png);
    const std::vector<std::tuple<json, std::vector<std::pair<std::string, std::string>>>> requests = {
        {{{"image", encoded}}, {{"seed", "<absent>"}, {"resolution", "<absent>"}, {"bg_removal", "<absent>"}, {"uv", "<absent>"}}},
        {{{"image", "data:image/png;base64," + encoded}, {"seed", 7}, {"resolution", 1024}, {"bg_removal", "rembg"}, {"uv", "xatlas"}}, {{"seed", "7"}, {"resolution", "1024"}, {"bg_removal", "rembg"}, {"uv", "xatlas"}}},
        {{{"image", encoded}, {"resolution", "1536"}, {"bg_removal", true}, {"uv", 1}}, {{"seed", "<absent>"}, {"resolution", "1536"}, {"bg_removal", "<absent>"}, {"uv", "<absent>"}}},
    };
    for (const auto& [request, fields] : requests) {
        Capture output;
        mesh->model_3d_generations(request, output.sink);
        bool same = upstream.last_target() == "/generate" && upstream.field("image") == k_png && upstream.image() == std::pair<std::string, std::string>{"input.png", "image/png"};
        for (const auto& [name, value] : fields) same = same && upstream.field(name) == value;
        check(same && output.bytes == k_glb && output.done == 0, "TRELLIS multipart upload and binary mesh output: " + request.dump().substr(0, 40));
    }
    const auto before = upstream.requests();
    for (const auto& request : {json::object(), json{{"image", 5}}}) {
        Capture output;
        mesh->model_3d_generations(request, output.sink);
        check(output.bytes.empty(), "TRELLIS writes nothing without a string image");
    }
    {
        Capture output;
        mesh->model_3d_generations({{"image", utils::JsonUtils::base64_encode("RIFF....WEBP")}}, output.sink);
        check(output.bytes == R"({"error":{"message":"unsupported image format: expected PNG, JPEG, BMP, or GIF","status_code":400,"type":"invalid_request_error"}})", "TRELLIS rejects an unsupported image");
    }
    {
        std::string message;
        try { Capture output; mesh->model_3d_generations({{"image", encoded}, {"seed", "7"}}, output.sink); } catch (const json::type_error& error) { message = error.what(); }
        check(message == "[json.exception.type_error.302] type must be number, but is string", "TRELLIS propagates a non-integer seed type error");
    }
    check(upstream.requests() == before, "rejected TRELLIS requests never reach the backend");
    {
        Capture output;
        mesh->model_3d_generations({{"image", encoded}, {"resolution", 1.5}}, output.sink);
        check(upstream.field("resolution") == "1" && output.bytes == k_glb, "TRELLIS truncates a fractional resolution like the wrapper");
    }
    {
        LogCapture capture("trellis-server");
        upstream.respond(500, "out of memory");
        Capture output;
        mesh->model_3d_generations({{"image", encoded}}, output.sink);
        check(output.bytes == R"json({"error":{"message":"generation failed (HTTP 500)","type":"backend_error"}})json" && capture.records == std::vector<std::string>{"Error generation failed (HTTP 500): out of memory"}, "TRELLIS backend failure envelope and log");
        upstream.respond(200, k_glb);
    }
    {
        EffectsFixture closed(upstream.port);
        auto orphan = trellis_server(closed);
        load(*orphan);
        const int dead = 1;
        class Access : public WrappedServer {
        public:
            static void port(WrappedServer& server, int value) { server.*(&Access::port_) = value; }
        };
        Access::port(*orphan, dead);
        LogCapture capture("trellis-server");
        Capture output;
        operation_adapter<IModel3DServer>(*orphan, BackendOperation::Model3DGeneration, OperationDelivery::Buffered, ModelType::MESH)->model_3d_generations({{"image", encoded}}, output.sink);
        const std::string error = std::string("CURL error: ") + curl_easy_strerror(CURLE_COULDNT_CONNECT);
        check(output.bytes == json{{"error", {{"message", error}, {"type", "backend_error"}}}}.dump() &&
              capture.records == std::vector<std::string>{"Error model_3d_generations failed: " + error}, "TRELLIS refused-connection exception envelope and log");
    }
    for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
        const std::string label = operation == BackendOperation::ChatCompletion ? "chat completion" : operation == BackendOperation::Completion ? "text completion" : "responses";
        check(server->dispatch_completion_operation(operation, json::object()) == json{{"error", {{"message", "trellis-server does not support " + label + ". Use the appropriate endpoint for this model type instead."}, {"type", "unsupported_operation"}, {"code", "model_not_applicable"}}}}, "TRELLIS keeps the generic rejection");
    }
    for (const auto* endpoint : {"/v1/chat/completions", "/v1/completions", "/v1/responses"}) {
        Capture output;
        server->forward_streaming_request(endpoint, "{ \"stream\" : true }", output.sink);
        check(upstream.last_target() == endpoint && upstream.last_body() == "{ \"stream\" : true }" && output.bytes == k_sse, "TRELLIS legacy streaming forwards original bytes");
    }
}

void check_registration() {
    const auto entry = trellis::configured_entry();
    const auto& d = entry->descriptor;
    const auto& compiled = registered_descriptor("trellis");
    descriptor(d, compiled);
    check(d.recipe == "trellis" && d.binary == "trellis-server" && d.supported_modes == std::vector<std::string>{"3d"} && d.options.size() == 2 && d.support.size() == 3 && d.config_defaults() == json{{"backend", "auto"}, {"args", ""}, {"vulkan_bin", "builtin"}, {"rocm_bin", "builtin"}, {"cuda_bin", "builtin"}}, "independent TRELLIS descriptor literals");
    check(entry->compatibility == TrustedCompatibility::LegacyNonChatSse && !entry->launch_compatibility.device_on_backend_selection && entry->launch_compatibility.load_log, "TRELLIS carries the legacy SSE and load-log attachments");
    check(trellis::capabilities() == CAP_MODEL_3D, "TRELLIS capability export");
    const auto* spec = trellis::spec();
    const std::vector<std::tuple<std::string, std::string, std::string>> assets = {
        {"vulkan", "windows", "trellis-vulkan-windows-x64.zip"}, {"rocm-stable", "windows", "trellis-rocm-windows-x64.zip"}, {"cuda", "linux", "trellis-cuda-linux-x64.tar.gz"},
        {"rocm", "linux", "trellis-rocm-linux-x64.tar.gz"}, {"rocm-nightly", "macos", "trellis-rocm-linux-x64.tar.gz"}, {"unsupported", "linux", "trellis-unsupported-linux-x64.tar.gz"},
    };
    for (const auto& [backend, os, filename] : assets) for (const auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
        const InstallContext context{*spec, spec->definition, "trellis", "trellis", backend, "", "ignored", "ignored", {os, "x86_64", ""}, phase};
        const auto params = spec->install_params_fn(context);
        check(params.repo == "pwilkin/trellis.cpp" && params.filename == filename && params.version_override.empty(), "TRELLIS asset " + backend + "/" + os);
    }
    const auto* ops = trellis::ops();
    const auto root = std::filesystem::temp_directory_path() / ("trellis-oracle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "snapshots/pinned");
    std::filesystem::create_directories(root / "refs");
    std::ofstream(root / "refs/main") << "pinned\n";
    CheckpointResolveContext context;
    context.model_cache_path = root.string();
    check(ops->resolve_checkpoint_path(model(), context) == (root / "snapshots/pinned").string(), "TRELLIS resolves the active snapshot");
    std::filesystem::remove(root / "refs/main");
    check(ops->resolve_checkpoint_path(model(), context) == root.string() && !ops->select_checkpoint_files("", {"a"}), "TRELLIS falls back to the cache and keeps default file selection");
    std::filesystem::remove_all(root);
}
}

int main() {
    Upstream upstream;
    check_registration();
    check_launch(upstream);
    check_accelerator_logging(upstream);
    check_mesh(upstream);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
