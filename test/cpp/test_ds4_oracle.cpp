#include "lemon/backends/ds4/ds4.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "llm_configured_fixture.h"

using namespace llm_oracle;

void* operator new(size_t size) { if (track_allocations) ++allocations; if (void* memory = std::malloc(size ? size : 1)) return memory; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
std::unique_ptr<WrappedServer> arm(Fixture& fixture, const std::string& level = "error") {
    return std::make_unique<ConfiguredServer>(ds4::configured_entry(), BackendContext{level}, ConfiguredServerServices{
        std::make_shared<Services>(fixture, true), fixture.effects().make_process});
}
void load(WrappedServer& server, const RecipeOptions& options = RecipeOptions("ds4", json::object()), const ModelInfo& model = ds4_model(), bool upgrade = false) {
    server.set_model_metadata("fixture", "fixture/repo", ModelType::LLM, DEVICE_NPU, options);
    server.load("fixture", model, options, upgrade); server.set_state(ModelState::READY);
}
}

int main() {
    const auto entry = ds4::configured_entry();
    check(entry->compatibility == TrustedCompatibility::None && entry->launch_compatibility.device_on_backend_selection == DEVICE_GPU && entry->launch_compatibility.posix_executable_loader_path && entry->launch_compatibility.load_log, "registered DS4 entry carries the GPU device, POSIX loader and load-log attachment");
    descriptor(entry->descriptor, ds4::descriptor);
    const auto& d = entry->descriptor;
    check(d.recipe == "ds4" && d.display_name == "DwarfStar4" && d.binary == "ds4-server" && d.config_section.empty() && d.default_device == DEVICE_GPU && d.slot_policy == SlotPolicy::Standard && d.uses_ctx_size && !d.selectable_backend && d.streams_model_from_storage, "independent DS4 metadata identities");
    check(d.options.size() == 1 && d.options[0].name == "ds4_args" && d.options[0].cli_flag == "--ds4-args" && d.options[0].default_value == "" && d.config_defaults() == json{{"args", ""}} && d.supported_modes == std::vector<std::string>{"chat"} && d.required_checkpoints == std::vector<std::string>{"main"}, "independent DS4 options/defaults/modes");
    check(d.support[0].backend == "rocm" && d.support[0].supported_os == std::set<std::string>{"linux"} && d.support[0].devices == DeviceConstraints{{"amd_gpu", {"gfx1151"}}} && ds4::capabilities() == 0, "independent DS4 support and unchanged capability export");
    auto external_document = json::parse(k_ds4_definition);
    external_document["metadata"]["options"][0]["cli_flag"] = "";
    auto external = BackendDefinitionStore().validate({"external.json", external_document.dump(), DefinitionOrigin::External});
    check(bool(external.definition), "external DS4 vocabulary remains valid");
    for (const auto compatibility : {BuiltinLaunchCompatibility{DEVICE_GPU, false}, BuiltinLaunchCompatibility{{}, true}, BuiltinLaunchCompatibility{DEVICE_GPU, true}, BuiltinLaunchCompatibility{{}, false, BuiltinLoadLog{}}}) {
        bool rejected = false;
        try { ConfiguredBackendEntry::make(external.definition, TrustedCompatibility::None, compatibility); } catch (const std::invalid_argument& error) { rejected = std::string(error.what()) == "External definitions cannot attach compiled compatibility"; }
        check(rejected, "external rejects every nonempty launch attachment");
    }
    check(bool(ConfiguredBackendEntry::make(external.definition)), "external permits empty launch attachment");
    Http http; http.health = 503;
    {
        for (const auto context : {json(nullptr), json(-1), json(0), json(4096), json(3.9)}) for (bool upgrade : {false, true}) {
            Fixture fixture(http.port);
            auto server = arm(fixture, "debug");
            RecipeOptions options("ds4", json::object()); if (!context.is_null()) options.set_option("ctx_size", context);
            load(*server, options, ds4_model(), upgrade);
            std::vector<std::string> expected = {"-m", u8"/models/Deep Seek/雪.gguf", "--host", "127.0.0.1", "--port", std::to_string(http.port)};
            if (context == 4096) expected.insert(expected.end(), {"-c", "4096"});
            if (context == 3.9) expected.insert(expected.end(), {"-c", "3"});
            expected.push_back("--ssd-streaming");
            check(fixture.command.args == expected && fixture.command.program == "/bundle/ds4-server", "literal DS4 context vector and always managed executable");
            check(fixture.events == std::vector<std::string>{u8"exists:/models/Deep Seek/雪.gguf", u8"exists:/models/Deep Seek/雪.gguf", "install:ds4:rocm", "managed:ds4:rocm", "port", "posix-loader", "process", "start"}, "literal DS4 final-exists/install/port/environment event order");
            check(server->get_device_type() == DEVICE_GPU && fixture.command.ready_endpoint == "/v1/models" && fixture.output.inherit && fixture.output.filter_health_logs && fixture.cwd.empty(), "DS4 device/readiness/output/cwd");
            if (context == 4096 && !upgrade) bindings(*server, fixture);
        }
        for (int scenario = 0; scenario < 8; ++scenario) {
            Fixture fixture(http.port); auto model = ds4_model();
            if (scenario < 6) model.resolved_paths["main"] = scenario == 1 ? "" : "/missing";
            model.checkpoints["main"] = scenario < 3 ? "/absolute" : scenario == 3 ? "relative" : scenario == 4 ? "/absent" : "";
            if (scenario < 4 || scenario == 6) fixture.paths.insert("/absolute");
            if (scenario == 7) fixture.paths.clear();
            auto server = arm(fixture);
            std::string message;
            try { load(*server, RecipeOptions("ds4", json::object()), model); } catch (const std::runtime_error& error) { message = error.what(); }
            if (scenario < 3) check(message.empty() && fixture.command.args[1] == "/absolute", "DS4 absolute fallback");
            else if (scenario >= 3 && scenario <= 5 || scenario == 7) check(message == "ds4: no local GGUF found for model 'fixture' (checkpoint: " + model.checkpoint() + ")" && server->get_device_type() == DEVICE_NPU && std::none_of(fixture.events.begin(), fixture.events.end(), [](const auto& e) { return e == "port" || e.rfind("install:", 0) == 0; }), "DS4 missing path literal before device/install/port");
            else check(message.empty() && fixture.command.args[1] == u8"/models/Deep Seek/雪.gguf", "resolved DS4 path wins");
        }
        for (const char* option : {"ds4_args", "ctx_size"}) {
            Fixture fixture(http.port); auto server = arm(fixture);
            RecipeOptions options("ds4", json::object()); options.set_option(option, json::object());
            bool rejected = false;
            try { load(*server, options); } catch (const json::type_error&) { rejected = true; }
            check(rejected && fixture.events.empty(), "DS4 option conversion fails before any effect");
        }
        for (const char* flag : {"--backend", "--cpu", "--ctx", "--cuda", "--host", "--metal", "--model", "--port", "--rocm", "-c", "-m"}) for (const char* suffix : {" value", "=value"}) {
            Fixture fixture(http.port); auto server = arm(fixture);
            std::string message;
            try { load(*server, RecipeOptions("ds4", {{"ds4_args", std::string(flag) + suffix}})); } catch (const std::invalid_argument& error) { message = error.what(); }
            check(message == std::string("Invalid custom ds4-server arguments:\nArgument '") + flag + "' is managed by Lemonade and cannot be overridden.\nReserved arguments: --backend, --cpu, --ctx, --cuda, --host, --metal, --model, --port, --rocm, -c, -m", "DS4 reserved flag exact invalid_argument literal");
            check(fixture.events == std::vector<std::string>{u8"exists:/models/Deep Seek/雪.gguf", u8"exists:/models/Deep Seek/雪.gguf", "install:ds4:rocm", "managed:ds4:rocm", "port"}, "DS4 custom rejection after port before environment");
        }
        {
            Fixture fixture(http.port); auto server = arm(fixture);
            bool device_at_install = false;
            fixture.on_install = [&] { device_at_install = server->get_device_type() == DEVICE_GPU; }; fixture.install_fails = true;
            bool original = false;
            try { load(*server); } catch (const std::logic_error& error) { original = std::string(error.what()) == "install witness"; }
            check(original && device_at_install && server->get_device_type() == DEVICE_GPU, "DS4 assignment before failing install");
        }
        for (const auto inherited : {std::optional<std::string>{}, std::optional<std::string>{""}, std::optional<std::string>{"old:a"}}) for (const char* level : {"info", "debug", "trace", "error"}) {
            Fixture fixture(http.port); fixture.inherited = inherited; auto server = arm(fixture, level);
            load(*server, RecipeOptions("ds4", {{"ds4_args", "--user 'two words' --user=again"}}));
            check(fixture.command.env == std::vector<std::pair<std::string, std::string>>{{"LD_LIBRARY_PATH", !inherited ? "/bundle" : "/bundle:" + *inherited}}, "literal DS4 loader absent/empty/present");
            check(fixture.output.inherit == (std::string(level) == "info" || std::string(level) == "debug") && fixture.output.filter_health_logs && fixture.cwd.empty(), "DS4 output exact levels");
            const auto& args = fixture.command.args;
            check(std::vector<std::string>(args.end() - 4, args.end()) == std::vector<std::string>{"--ssd-streaming", "--user", "two words", "--user=again"}, "DS4 streaming precedes duplicate custom args");
        }
        for (const char* custom : {"", "--user 'two words'", "--port 1"}) {
            Fixture fixture(http.port); auto server = arm(fixture, "info");
            LogCapture capture("DS4");
            try { load(*server, RecipeOptions("ds4", {{"ds4_args", custom}})); } catch (const std::invalid_argument&) {}
            const std::string starting = u8"Info Starting ds4-server (/bundle/ds4-server) for /models/Deep Seek/雪.gguf on port " + std::to_string(http.port);
            const std::vector<std::string> expected = std::string(custom).empty() ? std::vector<std::string>{starting} :
                std::string(custom) == "--port 1" ? std::vector<std::string>{} : std::vector<std::string>{"Debug Adding custom arguments: --user 'two words'", starting};
            check(capture.records == expected, "DS4 logs custom arguments at debug and the model path at startup, with no generic loading line");
        }
    }
    LaunchPlanner planner;
    for (const char* host : {"windows", "linux", "macos"}) {
        Fixture fixture(0); Services services(fixture, true); services.host.os = host;
        utils::HttpClient::set_default_timeout(37);
        const auto inputs = planner.prepare(*entry->definition, entry->spec, {"fixture", ds4_model(), RecipeOptions("ds4", json::object()), "error", false}, services);
        const auto plan = planner.plan(*entry->definition, inputs);
        check(plan.command.env == std::vector<std::pair<std::string, std::string>>{{"LD_LIBRARY_PATH", "/bundle:old"}} && plan.command.port == 0 && plan.readiness_timeout_seconds == 37 && plan.watchdog_enabled && plan.command.ready_endpoint == "/v1/models", "synthetic hosts POSIX attachment/global timeout/zero port");
        const auto before = fixture.events; planner.plan(*entry->definition, inputs); check(fixture.events == before, "DS4 pure plan");
    }
    utils::HttpClient::set_default_timeout(600);
    const std::vector<std::tuple<std::string, std::string, std::string, std::string>> cases = {
        {"rocm", "linux", "gfx1151", ""},
        {"cuda", "windows", "", "ds4 backend 'cuda' is not supported. Supported: rocm"},
        {"cuda", "linux", "gfx1151", "ds4 backend 'cuda' is not supported. Supported: rocm"},
        {"rocm", "windows", "", "ds4 backend 'rocm' publishes no build for windows; Linux only"},
        {"rocm", "linux", "", "ds4 backend 'rocm' requires a ROCm GPU, but no ROCm architecture was detected"},
        {"rocm", "linux", "gfx9999", "ds4 backend 'rocm' publishes no build for gfx9999; only gfx1151 is validated upstream"}
    };
    const auto* registered = try_get_spec_for_recipe("ds4");
    check(registered == ds4::spec() && registered == &entry->spec, "install path resolves the registered DS4 spec");
    for (const auto& [backend, os, arch, error] : cases) for (const char* version : {"b0001", "b0002"}) {
        const std::string host_os = os, host_arch = arch;
        std::vector<std::string> probes;
        const auto platform = install_platform(*registered, backend, {[&] { probes.push_back("os"); return host_os; }, [&] { probes.push_back("arch"); return host_arch; }});
        if (backend != "rocm") check(std::find(probes.begin(), probes.end(), "arch") == probes.end(), "unsupported backend never probes the architecture");
        else check(probes == (os != "linux" ? std::vector<std::string>{"os"} : std::vector<std::string>{"os", "arch"}), "unsupported OS probes the OS only; supported OS probes the OS then the architecture");
        std::string message; InstallParams params;
        for (auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
            InstallContext context{*registered, entry->definition, "ds4", "ds4", "ds4", backend, "requested", version, "b9999", platform, phase};
            message.clear();
            try { params = entry->spec.install_params_fn(context); } catch (const std::runtime_error& e) { message = e.what(); }
            check(message == error, "ordered install errors");
            if (error.empty()) check(params.repo == "lemonade-sdk/ds4-rocm" && params.filename == "ds4-" + std::string(phase == InstallPhase::Repository ? version : "b9999") + "-linux-rocm-gfx1151-x64.tar.gz", "distinct pin/asset versions literal");
        }
    }
    check(http.health_calls == 0 && http.model_calls > 0, "DS4 never probes health; non-JSON models readiness succeeds");
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
