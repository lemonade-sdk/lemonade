#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/thinksound/thinksound_server.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace thinksound_oracle;
using namespace lemon::backends;

namespace {
std::unique_ptr<WrappedServer> thinksound_server(EffectsFixture& fixture, const std::string& level) {
    return std::make_unique<ConfiguredServer>(thinksound::configured_entry(), BackendContext{level},
        ConfiguredServerServices{std::make_shared<FixtureLaunchServices>(fixture), fixture.effects().make_process});
}
}

int main() {
    HttpFixture http;
    {
        EffectsFixture fixture(http.port);
        auto server = thinksound_server(fixture, "debug");
        LogCapture capture("thinksound-server");
        server->load("fixture", model(), empty_options(), true);
        check(capture.records == std::vector<std::string>{"Info Loading model: fixture", "Info Starting /bin dir/ts-server on port " + std::to_string(http.port)}, "ThinkSound keeps its info loading and startup lines");
        check(fixture.events == std::vector<std::string>{u8"model:/model dir/雪\"trailing\\", "supported:thinksound", "validate:thinksound:vulkan", "external:thinksound:vulkan", "install:thinksound:vulkan", "managed:thinksound:vulkan", "port", "process", "start"}, "launch preparation ordered trace");
        check(fixture.command.program == "/bin dir/ts-server" && fixture.command.args == std::vector<std::string>{"--dir", u8"/model dir/雪\"trailing\\", "--host", "127.0.0.1", "--port", std::to_string(http.port)}, "argv vector boundaries");
        check(fixture.command.env.empty() && fixture.cwd.empty() && fixture.output.inherit && !fixture.output.filter_health_logs && fixture.command.ready_endpoint == "/health", "vulkan cwd output health");
        wire_oracle(*server, http);
        server->unload();
        check(fixture.stops == 1 && server->get_process_info().pid == 0 && server->get_launch_command().empty(), "unload clears process");
    }
    for (const auto* backend : {"rocm", "cuda", "rocm-stable", "rocm-nightly"}) {
        EffectsFixture fixture(http.port);
        fixture.external = "/external dir/ts-server";
        auto server = thinksound_server(fixture, "trace");
        server->load("fixture", model(), RecipeOptions("thinksound", {{"thinksound_backend", backend}}), false);
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
        check(fixture.command.env == expected, "ordered exact-backend environment");
        check(std::find(fixture.events.begin(), fixture.events.end(), "supported:thinksound") == fixture.events.end(), "explicit backend skips selection query");
        check(std::find(fixture.events.begin(), fixture.events.end(), std::string("install:thinksound:") + backend) == fixture.events.end(), "external binary skips install");
    }
    for (int failure = 0; failure < 4; ++failure) {
        EffectsFixture fixture(http.port);
        if (failure == 0) fixture.model_exists = false;
        if (failure == 1) fixture.supported.clear();
        if (failure == 2) fixture.port = 0;
        if (failure == 3) fixture.child_running = false;
        auto server = thinksound_server(fixture, "error");
        std::string message;
        try { server->load("fixture", model(), empty_options(), false); }
        catch (const std::exception& e) { message = e.what(); }
        if (failure == 0) check(message == "Model path not found for checkpoint: fixture/repo" && fixture.events.size() == 1, "missing artifact precedence");
        if (failure == 1) check(!message.empty() && fixture.events.size() == 2, "empty backend rejects before install");
        if (failure == 2) check(message == "Failed to find an available port", "zero port error");
        if (failure == 3) check(message == "thinksound-server failed to start" && fixture.stops == 1, "child exits before readiness");
    }
    const std::vector<std::pair<std::string, std::string>> installs = {
#ifdef _WIN32
        {"vulkan", "thinksound-vulkan-windows-x64.zip"},
        {"cuda", "thinksound-cuda-windows-x64.zip"},
        {"rocm", "thinksound-rocm-windows-x64.zip"},
        {"rocm-stable", "thinksound-rocm-windows-x64.zip"},
        {"rocm-any", "thinksound-rocm-windows-x64.zip"},
        {"unsupported", "thinksound-unsupported-windows-x64.zip"}
#else
        {"vulkan", "thinksound-vulkan-linux-x64.tar.gz"},
        {"cuda", "thinksound-cuda-linux-x64.tar.gz"},
        {"rocm", "thinksound-rocm-linux-x64.tar.gz"},
        {"rocm-stable", "thinksound-rocm-linux-x64.tar.gz"},
        {"rocm-any", "thinksound-rocm-linux-x64.tar.gz"},
        {"unsupported", "thinksound-unsupported-linux-x64.tar.gz"}
#endif
    };
    const auto* spec = thinksound::spec();
    for (const auto& [backend, filename] : installs) {
        for (const auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
            const InstallContext context{*spec, spec->definition, "thinksound", spec->recipe, spec->pin_recipe, backend, "", "ignored-tag", "ignored-tag", native_platform(), phase};
            const auto value = spec->install_params_fn(context);
            check(value.repo == "pwilkin/thinksound.cpp" && value.filename == filename && value.version_override.empty(), "permissive independent install asset");
        }
    }
    {
        EffectsFixture fixture(http.port);
        std::atomic<bool> cancel{true};
        auto server = thinksound_server(fixture, "error");
        server->set_load_cancel_flag(&cancel);
        std::string message;
        try { server->load("fixture", model(), empty_options(), false); }
        catch (const std::exception& e) { message = e.what(); }
        check(message == "thinksound-server failed to start" && fixture.stops == 1, "load cancellation stops child");
    }
    for (const auto inherited : {std::optional<std::string>{}, std::optional<std::string>{""}}) {
        EffectsFixture fixture(http.port);
        fixture.arch.clear();
        fixture.inherited = inherited;
        {
            auto server = thinksound_server(fixture, "error");
            server->load("fixture", model(), RecipeOptions("thinksound", {{"thinksound_backend", "rocm"}}), false);
#ifdef _WIN32
            const std::string expected = inherited ? "/bin dir;" : "/bin dir";
            const std::string key = "PATH";
#else
            const std::string expected = inherited ? "/bin dir:" : "/bin dir";
            const std::string key = "LD_LIBRARY_PATH";
#endif
            check(fixture.command.env == std::vector<std::pair<std::string, std::string>>{{key, expected}}, "empty arch absent versus empty inherited loader");
        }
        check(fixture.stops == 1, "destructor stops child");
    }
    const auto* ops = thinksound::ops();
    check(ops->select_checkpoint_files("ignored", {"clip-tokenizer.gguf", "dit-q8.gguf", "t5-bf16.gguf", "dit-bf16.gguf", "vae-f32.gguf", "metaclip-text-f32.gguf", "t5-tokenizer.gguf"}) == std::optional<std::vector<std::string>>{{"dit-bf16.gguf", "t5-bf16.gguf", "metaclip-text-f32.gguf", "vae-f32.gguf", "t5-tokenizer.gguf", "clip-tokenizer.gguf"}}, "six companion files ordered");
    check(ops->select_checkpoint_files("ignored", {"t5-bf16.gguf", "dit-bf16.gguf"}) == std::optional<std::vector<std::string>>{{"dit-bf16.gguf", "t5-bf16.gguf"}}, "missing companions skipped");
    check(ops->select_checkpoint_files("ignored", {}) == std::optional<std::vector<std::string>>{std::vector<std::string>{}}, "engaged empty selection");
    const auto root = std::filesystem::temp_directory_path() / ("thinksound-oracle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "snapshots/pinned");
    std::filesystem::create_directories(root / "refs");
    std::ofstream(root / "refs/main") << "pinned\n";
    backends::CheckpointResolveContext context;
    context.model_cache_path = root.string();
    check(ops->resolve_checkpoint_path(model(), context) == (root / "snapshots/pinned").string(), "active snapshot literal");
    std::filesystem::remove(root / "refs/main");
    check(ops->resolve_checkpoint_path(model(), context) == root.string(), "snapshot fallback literal");
    std::filesystem::remove_all(root);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
