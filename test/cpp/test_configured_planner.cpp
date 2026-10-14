#include "configured_fixture.h"
#include "lemon/backends/thinksound/thinksound.h"
#include "lemon/backends/thinksound/thinksound_server.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace thinksound_oracle;
using namespace lemon::backends;

int main() {
    const auto thinksound = entry();
    LaunchPlanner planner;
    for (const auto* backend : {"vulkan", "rocm", "cuda", "rocm-stable", "rocm-nightly"}) {
        EffectsFixture fixture(8123);
        FixtureLaunchServices services(fixture);
        RecipeOptions options("thinksound", {{"thinksound_backend", backend}});
        const auto inputs = planner.prepare(*thinksound->definition, thinksound->spec, {"fixture", model(), options, "debug", true}, services);
        const auto plan = planner.plan(*thinksound->definition, inputs);
        std::vector<std::string> expected = {u8"model:/model dir/雪\"trailing\\", std::string("validate:thinksound:") + backend,
            std::string("external:thinksound:") + backend, std::string("install:thinksound:") + backend,
            std::string("managed:thinksound:") + backend, "port"};
        if (std::string(backend) == "rocm") expected.insert(expected.end(), {"arch", "runtime:gfx-fixture", "inherited"});
        if (std::string(backend) == "cuda") expected.insert(expected.end(), {"inherited", "cuda:thinksound-server"});
        check(fixture.events == expected, "independent preparation/environment trace");
        check(plan.command.program == "/bin dir/ts-server" && plan.command.args == std::vector<std::string>{"--dir", u8"/model dir/雪\"trailing\\", "--host", "127.0.0.1", "--port", "8123"}, "independent argv vector");
#ifdef _WIN32
        const std::string key = "PATH", rocm = "/runtime/core:/runtime/libs;/bin dir;old", cuda = "/bin dir;old";
#else
        const std::string key = "LD_LIBRARY_PATH", rocm = "/runtime/core:/runtime/libs:/bin dir:old", cuda = "/bin dir:old";
#endif
        std::vector<std::pair<std::string, std::string>> env;
        if (std::string(backend) == "rocm") env = {{key, rocm}};
        if (std::string(backend) == "cuda") env = {{key, cuda}, {"CUDA_VISIBLE_DEVICES", "2,0"}, {"__NV_PRIME_RENDER_OFFLOAD", "1"}};
        check(plan.command.env == env, "independent environment vectors");
        check(plan.command.port == 8123 && plan.command.ready_endpoint == "/health" && plan.readiness_timeout_seconds == 600 && plan.watchdog_enabled && plan.working_directory.empty() && plan.output.inherit && !plan.output.filter_health_logs, "independent lifecycle launch parameters");
        const auto events = fixture.events;
        planner.plan(*thinksound->definition, inputs);
        check(fixture.events == events, "plan has no effects");
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        EffectsFixture fixture(8123);
        if (scenario == 0) fixture.model_exists = false;
        if (scenario == 1) fixture.supported.clear();
        if (scenario == 2) fixture.port = 0;
        if (scenario == 3) fixture.external = "/external dir/ts-server";
        FixtureLaunchServices services(fixture);
        std::string message;
        try { planner.prepare(*thinksound->definition, thinksound->spec, {"fixture", model(), empty_options(), "error", false}, services); }
        catch (const std::exception& error) { message = error.what(); }
        if (scenario == 0) check(message == "Model path not found for checkpoint: fixture/repo" && fixture.events.size() == 1, "artifact failure before selection");
        if (scenario == 1) check(!message.empty() && fixture.events.size() == 2, "empty supported failure before install");
        if (scenario == 2) check(message == "Failed to find an available port", "zero port literal error");
        if (scenario == 3) check(message.empty() && fixture.events == std::vector<std::string>{u8"model:/model dir/雪\"trailing\\", "supported:thinksound", "validate:thinksound:vulkan", "external:thinksound:vulkan", "binary-exists:/external dir/ts-server", "port"}, "external binary skips install trace");
    }
    const std::vector<std::tuple<std::string, std::string, std::string>> installs = {
        {"windows", "vulkan", "thinksound-vulkan-windows-x64.zip"}, {"linux", "cuda", "thinksound-cuda-linux-x64.tar.gz"},
        {"macos", "unsupported", "thinksound-unsupported-linux-x64.tar.gz"}, {"windows", "rocm-stable", "thinksound-rocm-windows-x64.zip"},
        {"linux", "rocm-any", "thinksound-rocm-linux-x64.tar.gz"}
    };
    for (const auto& [os, backend, filename] : installs) {
        for (const auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
            InstallContext context{thinksound->spec, thinksound->definition, "thinksound", "thinksound", "thinksound", backend,
                "user-tag", "pin-tag", "resolved-tag", {os, "x86_64", ""}, phase};
            const auto params = thinksound->spec.install_params_fn(context);
            check(params.repo == "pwilkin/thinksound.cpp" && params.filename == filename && params.version_override.empty(), "independent install assets in both phases");
        }
    }
    std::vector<std::string> versions;
    static std::vector<std::string>* captured = &versions;
    const auto legacy = +[](const std::string& backend, const std::string& version) { captured->push_back(backend + ":" + version); return InstallParams{"repo", "asset", ""}; };
    BackendSpec legacy_spec("legacy-install", "legacy-bin", legacy);
    InstallContext legacy_context{legacy_spec, nullptr, "legacy-public", "legacy-install", "legacy-pin", "backend", "user", "pinned", "resolved", native_platform(), InstallPhase::Repository};
    legacy_spec.install_params_fn(legacy_context);
    legacy_context.phase = InstallPhase::Asset;
    legacy_spec.install_params_fn(legacy_context);
    check(versions == std::vector<std::string>{"backend:pinned", "backend:resolved"}, "legacy two-pass adapter values");
    auto alternate_doc = json::parse(k_thinksound_definition);
    alternate_doc["id"] = "other"; alternate_doc["recipe"] = "other";
    alternate_doc["metadata"]["binary"] = "other-bin";
    alternate_doc["install"]["install_recipe"] = "other-install"; alternate_doc["install"]["pin_recipe"] = "other-pin";
    alternate_doc["install"]["asset_backend"] = "identity";
    alternate_doc["install"]["cases"] = json::array({json{{"repo", "other/repo"}, {"filename", {{"concat", json::array({"asset-", json{{"ref", "/install/version"}}})}}}, {"version_override", "fixed-release"}}});
    alternate_doc["artifacts"]["main"]["files"] = {"second", "first"};
    alternate_doc["operations"][0]["transport"]["path"] = "/other-generate";
    alternate_doc["install"]["cases"][0]["sha256"] = std::string(64, 'a');
    const auto other = entry(alternate_doc.dump());
    for (int i = 0; i < 3; ++i) {
        InstallContext context{other->spec, other->definition, "other", "other-install", "other-pin", "vulkan", "user", "pin", "resolved", {"linux", "x86_64", ""}, InstallPhase::Repository};
        auto params = other->spec.install_params_fn(context);
        check(params.repo == "other/repo" && params.filename == "asset-pin" && params.version_override == "fixed-release" && params.sha256 == std::string(64, 'a'), "other repository phase isolation");
        context.phase = InstallPhase::Asset; params = other->spec.install_params_fn(context);
        check(params.filename == "asset-resolved" && other->spec.recipe == "other-install" && other->spec.pin_recipe == "other-pin" && other->spec.binary == "other-bin", "other asset/spec identity isolation");
        check(other->ops->select_checkpoint_files("any", {"first", "second", "dit-bf16.gguf"}) == std::optional<std::vector<std::string>>{{"second", "first"}} && other->definition->operations[0].path == "/other-generate", "other file/operation isolation");
        context.definition = thinksound->definition;
        bool rejected = false;
        try { other->spec.install_params_fn(context); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "cross-definition ownership rejected");
        check(thinksound->ops->select_checkpoint_files("any", {"dit-bf16.gguf", "second"}) == std::optional<std::vector<std::string>>{{"dit-bf16.gguf"}} && thinksound->definition->operations[0].path == "/generate", "ThinkSound stays isolated after alternating calls");
    }
    const auto root = std::filesystem::temp_directory_path() / ("configured-snapshot-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "refs"); std::filesystem::create_directories(root / "snapshots/pinned");
    std::ofstream(root / "refs/main") << "pinned\n";
    CheckpointResolveContext cache; cache.model_cache_path = root.string();
    for (const auto* type : {"", "main", "auxiliary"}) {
        cache.type = type;
        check(thinksound->ops->resolve_checkpoint_path(model(), cache) == (root / "snapshots/pinned").string(), "primary snapshot independent of undeclared context role");
    }
    std::filesystem::remove(root / "refs/main");
    check(thinksound->ops->resolve_checkpoint_path(model(), cache) == root.string(), "snapshot original cache fallback");
    std::filesystem::remove_all(root);
    check(thinksound->descriptor.config_defaults() == backends::thinksound::descriptor.config_defaults(), "descriptor config defaults unchanged");
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
