#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/ryzenai/ryzenai_server.h"
#include "lemon/backends/thinksound/thinksound_server.h"
#include "llm_configured_fixture.h"

using namespace llm_oracle;

void* operator new(size_t size) { if (track_allocations) ++allocations; if (void* memory = std::malloc(size ? size : 1)) return memory; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, size_t) noexcept { std::free(memory); }

namespace {
ModelInfo ryzen_model() { auto model = ds4_model(); model.resolved_paths["main"] = u8"/models/Ryzen AI/雪"; return model; }
std::unique_ptr<WrappedServer> arm(Fixture& fixture, const std::string& level = "error") {
    fixture.executable = "/bundle/ryzenai-server"; fixture.paths.insert(u8"/models/Ryzen AI/雪");
    return std::make_unique<ConfiguredServer>(ryzenai::configured_entry(), BackendContext{level},
        ConfiguredServerServices{std::make_shared<Services>(fixture), fixture.effects().make_process});
}
void load(WrappedServer& server, const RecipeOptions& options, const ModelInfo& model = ryzen_model(), bool upgrade = false) {
    server.set_model_metadata("fixture", "fixture/repo", ModelType::LLM, DEVICE_CPU, options);
    server.load("fixture", model, options, upgrade); server.set_state(ModelState::READY);
}
}
int main() {
    const auto entry = ryzenai::configured_entry();
    check(entry->compatibility == TrustedCompatibility::None, "registered RyzenAI entry has no compiled compatibility");
    descriptor(entry->descriptor, registered_descriptor("ryzenai-llm"));
    const auto& d = entry->descriptor;
    check(d.recipe == "ryzenai-llm" && d.display_name == "Ryzen AI LLM" && d.config_section == "ryzenai" && d.default_device == DEVICE_NPU && d.slot_policy == SlotPolicy::ExclusiveNpu && d.uses_ctx_size && !d.selectable_backend && !d.streams_model_from_storage, "independent RyzenAI identities/policy");
    check(d.options.empty() && d.config_defaults() == json{{"server_bin", "builtin"}} && d.supported_modes == std::vector<std::string>{"chat"} && d.required_checkpoints == std::vector<std::string>{"main"} && d.web_display_name == "Ryzen AI SW NPU", "independent RyzenAI options/defaults/modes");
    check(d.support.size() == 1 && d.support[0].backend == "npu" && d.support[0].tier == BackendTier::Core && d.support[0].supported_os == std::set<std::string>{"windows"} && d.support[0].devices == DeviceConstraints{{"amd_npu", {"XDNA2"}}} && ryzenai::capabilities() == 0, "independent support and capability export");
    check(entry->spec.recipe == "ryzenai-server" && entry->spec.public_recipe == "ryzenai-llm" && entry->spec.pin_recipe == "ryzenai-llm" && !entry->spec.supports_split_archive && !entry->launch_compatibility.device_on_backend_selection && !entry->launch_compatibility.posix_executable_loader_path && entry->launch_compatibility.load_log, "split spec identities; load-log-only launch attachment");
    check(entry->definition->records["metadata"]["binary"] == json{{"windows", "ryzenai-server.exe"}, {"linux", "ryzenai-server"}, {"macos", "ryzenai-server"}}, "independent platform binary strings");
    for (const char* backend : {"npu", "unexpected", ""}) for (const char* version : {"b0001", "latest", ""}) {
        for (const char* os : {"windows", "linux", "macos", "unexpected"}) for (auto phase : {InstallPhase::Repository, InstallPhase::Asset}) {
            InstallContext context{entry->spec, entry->definition, "ryzenai-llm", "ryzenai-llm", backend, version, "pin", "resolved", {os, "x86_64", ""}, phase};
            const auto params = entry->spec.install_params_fn(context);
            check(params.repo == "lemonade-sdk/ryzenai-server" && params.filename == "ryzenai-server.zip" && params.version_override.empty(), "permissive install across OS/backend/version/phases");
        }
    }
    Http http; http.models = 503;
    for (const char* missing : {"", "/missing"}) {
        Fixture fixture(http.port); auto server = arm(fixture, "info"); auto model = ryzen_model();
        if (*missing) model.resolved_paths["main"] = missing;
        LogCapture capture("RyzenAI");
        try { load(*server, RecipeOptions("ryzenai-llm", {{"ctx_size", 4096}}), model); } catch (const std::runtime_error&) {}
        std::vector<std::string> expected{"Debug Loading model: fixture", "Debug Found ryzenai-server at: /bundle/ryzenai-server"};
        if (!*missing) expected.insert(expected.end(), {u8"Debug Model path: /models/Ryzen AI/雪",
            u8"Debug Starting: \"/bundle/ryzenai-server\" \"-m\" \"/models/Ryzen AI/雪\" \"--port\" \"" + std::to_string(http.port) + "\" \"--ctx-size\" \"4096\""});
        check(capture.records == expected, "RyzenAI loading/executable/model-path/command diagnostics stay at debug in wrapper order");
    }
    {
        for (const auto context : {json(nullptr), json(-1), json(0), json(4096), json(3.9)}) for (const char* level : {"debug", "trace", "info", "error", ""}) for (bool upgrade : {false, true}) {
            Fixture fixture(http.port); auto server = arm(fixture, level);
            RecipeOptions options("ryzenai-llm", json::object()); if (!context.is_null()) options.set_option("ctx_size", context);
            load(*server, options, ryzen_model(), upgrade);
            const std::string ctx = context.is_null() || context == -1 ? "-1" : context == 0 ? "0" : context == 3.9 ? "3" : "4096";
            std::vector<std::string> expected{"-m", u8"/models/Ryzen AI/雪", "--port", std::to_string(http.port), "--ctx-size", ctx};
            if (std::string(level) == "debug") expected.push_back("--verbose");
            check(fixture.command.args == expected && fixture.command.program == "/bundle/ryzenai-server", "literal RyzenAI context vector including zero/negative/fractional");
            check(fixture.events == std::vector<std::string>{"install:ryzenai-llm:npu", "managed:ryzenai-server:npu", u8"exists:/models/Ryzen AI/雪", "port", "process", "start"}, "literal RyzenAI install/lookup/model/port/start ordering; no executable stat or support query");
            check(fixture.command.env.empty() && fixture.cwd.empty() && fixture.output.inherit == (std::string(level) == "debug") && fixture.output.filter_health_logs && server->get_device_type() == DEVICE_CPU && fixture.command.ready_endpoint == "/health", "exact debug conversion/environment/output/device preservation/readiness");
            if (context == 4096 && std::string(level) == "debug" && !upgrade) bindings(*server, fixture);
        }
        for (int scenario = 0; scenario < 6; ++scenario) {
            Fixture fixture(http.port); auto server = arm(fixture); auto model = ryzen_model();
            model.resolved_paths["main"] = scenario == 4 ? "" : "/missing";
            if (scenario == 0) fixture.install_fails = true;
            if (scenario == 1) fixture.lookup_fails = true;
            if (scenario == 2) fixture.executable.clear();
            RecipeOptions options("ryzenai-llm", json::object());
            if (scenario == 5) options.set_option("ctx_size", json::object());
            std::string message, kind;
            try { load(*server, options, model); }
            catch (const json::type_error& error) { kind = "json"; message = error.what(); }
            catch (const std::logic_error& error) { kind = "logic"; message = error.what(); }
            catch (const std::runtime_error& error) { kind = "runtime"; message = error.what(); }
            if (scenario == 0) check(kind == "logic" && message == "install witness" && fixture.events == std::vector<std::string>{"install:ryzenai-llm:npu"}, "RyzenAI install exception masks missing model");
            if (scenario == 1) check(kind == "logic" && message == "lookup witness" && fixture.events == std::vector<std::string>{"install:ryzenai-llm:npu", "managed:ryzenai-server:npu"}, "RyzenAI managed exception not wrapped");
            if (scenario == 2) check(kind == "runtime" && message == "RyzenAI-Server executable not found even after installation attempt" && fixture.events.size() == 2, "RyzenAI empty executable before model check");
            if (scenario == 3 || scenario == 4) check(kind == "runtime" && message == "Model path does not exist: " + model.resolved_path() && fixture.events.size() == (scenario == 3 ? 3 : 2), "RyzenAI missing path literal includes empty candidate");
            if (scenario == 5) check(kind == "json" && fixture.events.empty(), "RyzenAI ctx conversion before effects");
            check(server->get_device_type() == DEVICE_CPU, "RyzenAI never reassigns device on failure");
        }
    }
    Fixture fixture(0); fixture.executable = "nonempty-unstatable"; fixture.paths.insert(u8"/models/Ryzen AI/雪"); Services services(fixture);
    LaunchPlanner planner; utils::HttpClient::set_default_timeout(37);
    const auto inputs = planner.prepare(*entry->definition, entry->spec, {"fixture", ryzen_model(), RecipeOptions("ryzenai-llm", json::object()), "trace"}, services);
    const auto plan = planner.plan(*entry->definition, inputs);
    check(plan.command.port == 0 && plan.command.program == "nonempty-unstatable" && plan.readiness_timeout_seconds == 600 && plan.command.ready_endpoint == "/health" && plan.watchdog_enabled && !plan.output.inherit, "RyzenAI nonempty executable/no second stat, zero port, fixed 600 timeout");
    utils::HttpClient::set_default_timeout(300);
    const auto root = std::filesystem::temp_directory_path() / utils::path_from_utf8(u8"ryzenai-oracle-雪-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto verify = [&](const std::string& path, const std::string& cache_expected, const std::string& import_expected) {
        CheckpointResolveContext context; context.model_cache_path = path;
        const BackendOps* ops = ryzenai::ops();
        check(ops->resolve_checkpoint_path(ryzen_model(), context) == cache_expected && ops->find_imported_checkpoint(path) == import_expected, "independent cache/import sentinel expectations");
        check(!ops->select_checkpoint_files("any", {"model.onnx"}), "sentinel retains inherited file selection");
    };
    const auto utf8 = [](const auto& path) { return utils::path_to_utf8(path); };
    verify(utf8(root / "absent"), utf8(root / "absent"), "");
    verify(utf8(root) + "/", utf8(root) + "/", "");
    std::filesystem::create_directory(root / "genai_config.json");
    std::ofstream(root / "wrong.json") << "ignored";
    verify(utf8(root), utf8(root), "");
    std::filesystem::remove(root / "genai_config.json");
    std::ofstream(root / "genai_config.json") << "not parsed";
    verify(utf8(root), utf8(root), utf8(root));
    std::filesystem::remove(root / "genai_config.json");
    std::filesystem::create_directories(root / utils::path_from_utf8(u8"nested/雪"));
    std::ofstream(root / utils::path_from_utf8(u8"nested/雪/genai_config.json")) << "not parsed";
    verify(utf8(root), utf8(root / utils::path_from_utf8(u8"nested/雪")), utf8(root / utils::path_from_utf8(u8"nested/雪")));
    std::filesystem::remove_all(root / "nested");
    std::filesystem::create_directories(root / "snapshots/z-first"); std::filesystem::create_directories(root / "snapshots/a-second");
    std::ofstream(root / "snapshots/z-first/genai_config.json") << "z"; std::ofstream(root / "snapshots/a-second/genai_config.json") << "a";
    std::vector<std::string> enumeration;
    for (const auto& child : std::filesystem::directory_iterator(root / "snapshots", hf_cache::dir_options())) enumeration.push_back(child.path().filename().string());
    check(enumeration.size() == 2 && std::set<std::string>(enumeration.begin(), enumeration.end()) == std::set<std::string>{"z-first", "a-second"}, "hand-authored snapshot candidate names");
    const std::string first = enumeration.at(0) == "z-first" ? "z-first" : "a-second";
    const std::string other = first == "z-first" ? "a-second" : "z-first";
    std::printf("Fixture directory enumeration: %s, %s; refs/main=%s\n", enumeration[0].c_str(), enumeration[1].c_str(), other.c_str());
    std::filesystem::create_directories(root / "refs"); std::ofstream(root / "refs/main") << other;
    verify(utf8(root), utf8(root / "snapshots" / first), utf8(root / "snapshots" / first));
    {
        bool threw = false;
        try { ryzenai::ops()->find_imported_checkpoint(utf8(root / "wrong.json")); } catch (const std::filesystem::filesystem_error&) { threw = true; }
        check(threw, "throwing recursive iterator preserves regular-file-root failure");
    }
    std::filesystem::create_directories(root / "custom");
    std::ofstream(root / "custom/checkpoint.marker") << "opaque";
    auto custom_document = json::parse(k_ryzenai_definition);
    custom_document["artifacts"]["main"]["sentinel"] = "checkpoint.marker";
    const auto custom = definition_entry(custom_document.dump());
    check(custom->ops->find_imported_checkpoint(utf8(root)) == utf8(root / "custom"), "shared sentinel primitive uses configured filename");
    std::filesystem::remove_all(root);
    for (const char* axis : {"cache_resolution", "import_resolution"}) {
        for (const auto sentinel : {json(nullptr), json(""), json("."), json(".."), json("a/b"), json("a\\b"), json("C:sentinel"), json(std::string("a\0b", 3))}) {
            auto document = json::parse(k_ryzenai_definition); document["artifacts"]["main"][axis] = "sentinel_directory";
            if (sentinel.is_null()) document["artifacts"]["main"].erase("sentinel"); else document["artifacts"]["main"]["sentinel"] = sentinel;
            check(!BackendDefinitionStore().validate({"invalid.json", document.dump(), DefinitionOrigin::Builtin}).definition, "sentinel requires nonempty portable single filename");
        }
        for (const auto selection : {json(nullptr), json("unique")}) {
            auto document = json::parse(k_ryzenai_definition); document["artifacts"]["main"][axis] = "sentinel_directory";
            if (selection.is_null()) document["artifacts"]["main"].erase("selection"); else document["artifacts"]["main"]["selection"] = selection;
            check(!BackendDefinitionStore().validate({"invalid.json", document.dump(), DefinitionOrigin::Builtin}).definition, "sentinel rejects missing/unique selection");
        }
    }
    auto hooked = json::parse(k_ryzenai_definition); hooked["artifacts"]["main"]["strategy"] = "hook"; hooked["artifacts"]["main"]["hook"] = "ryzenai.artifacts.v1";
    check(!BackendDefinitionStore().validate({"hook.json", hooked.dump(), DefinitionOrigin::Builtin}).definition, "artifact hooks still rejected");
    const auto thinksound = lemon::backends::thinksound::configured_entry();
    const auto ds4 = lemon::backends::ds4::configured_entry();
    for (int i = 0; i < 3; ++i) {
        auto ts = thinksound->factory(BackendContext{"error"}); auto ds = ds4->factory(BackendContext{"error"}); auto ryzen = entry->factory(BackendContext{"error"});
        check(thinksound->spec.recipe == "thinksound" && thinksound->spec.binary == "ts-server" && ds4->spec.recipe == "ds4" && ds4->spec.binary == "ds4-server" && entry->spec.recipe == "ryzenai-server", "three independent factory/spec owners");
        check(ts->supports_operation(BackendOperation::AudioGeneration, OperationDelivery::Buffered, ModelType::AUDIO_GENERATION) && !ds->supports_operation(BackendOperation::AudioGeneration, OperationDelivery::Buffered, ModelType::AUDIO_GENERATION) && ryzen->supports_operation(BackendOperation::Responses, OperationDelivery::Streaming, ModelType::MESH), "three independent operation sets and mode masks");
        check(thinksound->ops->select_checkpoint_files("", {"dit-bf16.gguf", "other"}) == std::optional<std::vector<std::string>>{{"dit-bf16.gguf"}} && !ds4->ops->select_checkpoint_files("", {"other"}) && entry->ops->find_imported_checkpoint("/absent") == "", "three independent artifact policies");
        check(!thinksound->launch_compatibility.device_on_backend_selection && !thinksound->launch_compatibility.posix_executable_loader_path && !entry->launch_compatibility.device_on_backend_selection, "DS4 attachment never leaks into other entries");
    }
    const std::vector<std::tuple<std::shared_ptr<const ConfiguredBackendEntry>, std::string, std::string, std::string, std::string>> installations = {
        {thinksound, "thinksound", "vulkan", "pwilkin/thinksound.cpp", "thinksound-vulkan-linux-x64.tar.gz"},
        {ds4, "ds4", "rocm", "lemonade-sdk/ds4-rocm", "ds4-pin-linux-rocm-gfx1151-x64.tar.gz"},
        {entry, "ryzenai-llm", "npu", "lemonade-sdk/ryzenai-server", "ryzenai-server.zip"}
    };
    for (int pass = 0; pass < 3; ++pass) for (const auto& [owner, public_recipe, backend, repository, filename] : installations) {
        InstallContext context{owner->spec, owner->definition, public_recipe, public_recipe, backend, "requested", "pin", "resolved", {"linux", "x86_64", "gfx1151"}, InstallPhase::Repository};
        const auto params = owner->spec.install_params_fn(context);
        check(params.repo == repository && params.filename == filename, "alternating three-entry repositories and assets independent literals");
        check(owner->definition->operations[0].path == (public_recipe == "thinksound" ? "/generate" : "/v1/chat/completions"), "alternating three-entry independent operation paths");
    }
    for (const auto* text : {k_thinksound_definition, k_ds4_definition, k_ryzenai_definition}) {
        BackendCreateFn expired;
        { const auto temporary = definition_entry(text); expired = temporary->factory; }
        bool rejected = false;
        try { expired(BackendContext{"error"}); } catch (const std::logic_error&) { rejected = true; }
        check(rejected, "all three expired factories reject without dangling state");
    }
    check(http.model_calls == 0 && http.health_calls > 0, "RyzenAI only health readiness on unsupported synthetic host");
    std::printf("%d/%d checks passed\n", checks - failures, checks); return failures ? 1 : 0;
}
