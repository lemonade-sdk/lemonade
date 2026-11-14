#pragma once

#include "llamacpp_v2_parity.h"
#include "support/test_state_isolation.h"

#include <filesystem>

namespace runtime_fixture::llama_corpus {
namespace fs = std::filesystem;

inline Json read(const std::string& name) {
    std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + name + ".json");
    return Json::parse(input);
}
inline Json normalize(Json value, const std::string& temp = "") {
    if (value.is_string()) {
        auto text = value.get<std::string>();
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{{"/oracle", "<ROOT>"}, {temp, "<TEMP>"}}) {
            if (pair.first.empty()) continue;
            std::size_t at = 0;
            while ((at = text.find(pair.first, at)) != std::string::npos) {
                text.replace(at, pair.first.size(), pair.second); at += pair.second.size();
            }
        }
        return text;
    }
    if (value.is_structured()) for (auto& item : value) item = normalize(std::move(item), temp);
    return value;
}
inline ModelInfo model(const Json& input) {
    ModelInfo info;
    info.checkpoints["main"] = input.value("checkpoint", "fixture/repo:Q4_0");
    info.checkpoints["draft"] = input.value("draft_checkpoint", "");
    info.resolved_paths["main"] = input.value("main", "/oracle/models/main.gguf");
    info.resolved_paths["mmproj"] = input.value("mmproj", "");
    info.resolved_paths["draft"] = input.value("draft", "");
    info.labels = input.value("labels", std::vector<std::string>{});
    info.extras["hf_load"] = input.value("hf_load", false);
    const auto type = input.value("type", "llm");
    info.type = type == "embedding" ? ModelType::EMBEDDING : type == "reranking" ? ModelType::RERANKING : ModelType::LLM;
    return info;
}
class Sources final : public LaunchServices {
public:
    Json facts;
    mutable Json events = Json::array();
    PlatformInfo host;
    Sources(const Json& input, const std::string& view) : facts(input.value("facts", Json::object())) {
        host.os = view.rfind("windows", 0) == 0 ? "windows" : view.rfind("macos", 0) == 0 ? "macos" : "linux";
        host.machine_arch = view.find("arm64") == std::string::npos ? "x86_64" : "aarch64";
    }
    PlatformInfo platform() const override { return host; }
    std::string rocm_channel() const override {
        events.push_back("runtime-config");
        return facts.value("no_config", false) ? "stable" : facts.value("channel", "stable");
    }
    bool exists(const std::string& path) const override {
        events.push_back("exists:" + path);
        const auto files = facts.value("exists", Json::array());
        return std::find(files.begin(), files.end(), path) != files.end();
    }
    std::vector<std::string> supported_backends(const std::string& recipe) const override {
        events.push_back("supported:" + recipe);
        return host.os == "macos" ? std::vector<std::string>{"metal"} : std::vector<std::string>{"vulkan", "rocm", "cuda", "cpu"};
    }
    void validate_backend(const std::string& recipe, const std::string& backend) const override {
        if (backend == "auto") return;
        if (backend == "system") {
            events.push_back("os");
            if (host.os == "linux") return;
        }
        const auto supported = supported_backends(recipe);
        if (std::find(supported.begin(), supported.end(), backend) != supported.end()) return;
        std::string allowed = "auto";
        for (const auto& value : supported) allowed += ", " + value;
        throw std::invalid_argument("'llamacpp.backend' must be one of: " + allowed);
    }
    void install_backend(const std::string& recipe, const std::string& backend) override {
        events.push_back("install:" + recipe + ":" + backend);
        if (facts.contains("install_error")) throw std::runtime_error(facts.at("install_error").get<std::string>());
    }
    std::string external_binary(const BackendSpec& spec, const std::string& backend) const override {
        events.push_back("external-binary:" + spec.recipe + ":" + backend);
        if (facts.contains("binary_error")) throw std::runtime_error(facts.at("binary_error").get<std::string>());
        return facts.value("external_binary", "");
    }
    std::string managed_binary(const BackendSpec& spec, const std::string& backend) const override {
        if (backend == "system") {
            events.push_back("path-executable:" + spec.binary);
            if (facts.value("path_executable", "/oracle/system/" + spec.binary).empty()) throw std::runtime_error(spec.binary + " not found in PATH");
            return spec.binary;
        }
        const auto external = external_binary(spec, backend);
        if (!external.empty()) return external;
        const auto directory = "/oracle/bundle/" + backend;
        events.push_back("install-directory:" + spec.recipe + ":" + backend);
        events.push_back("installed-binary:" + directory + ":" + spec.binary);
        const auto binary = facts.value("installed_binary", directory + "/" + spec.binary + (host.os == "windows" ? ".exe" : ""));
        if (binary.empty()) throw std::runtime_error(spec.binary + " not found in install directory: " + directory);
        return binary;
    }
    std::string staging_directory(const std::string& program) const override { return fs::path(program).parent_path().string(); }
    int allocate_port() override { events.push_back("port"); return 12345; }
    int find_free_port(int start) override { return start; }
    long default_timeout_seconds() const override { return 600; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    rt::EnvironmentSources environment_sources() {
        return {
            [this](typed::ParentKey key) -> std::optional<std::string> {
                static const std::map<typed::ParentKey, std::string> names{{typed::ParentKey::Path, "PATH"}, {typed::ParentKey::LlamaDevice, "LLAMA_ARG_DEVICE"},
                    {typed::ParentKey::MetalNoResidency, "GGML_METAL_NO_RESIDENCY"}, {typed::ParentKey::Home, "HOME"}, {typed::ParentKey::LibraryPath, "LD_LIBRARY_PATH"},
                    {typed::ParentKey::CudaVisibleDevices, "CUDA_VISIBLE_DEVICES"}, {typed::ParentKey::PrimeRenderOffload, "__NV_PRIME_RENDER_OFFLOAD"}};
                const auto name = names.at(key); events.push_back("env:" + name);
                const auto env = facts.value("env", Json::object());
                return env.contains(name) ? std::optional<std::string>(env.at(name)) : std::nullopt;
            },
            [this](typed::RuntimeQuery query) -> std::string {
                switch (query) {
                    case typed::RuntimeQuery::RocmArchitecture: events.push_back("rocm-arch"); return facts.value("rocm_arch", "gfx1151");
                    case typed::RuntimeQuery::CudaArchitecture: events.push_back("cuda-arch"); return facts.value("cuda_arch", "sm_120");
                    case typed::RuntimeQuery::CudaVisibleDevices: events.push_back("cuda-visible:" + facts.value("cuda_arch", "sm_120")); return facts.value("cuda_visible_devices", "0,2");
                    case typed::RuntimeQuery::TheRockDirectory: {
                        events.push_back("runtime-paths:" + facts.value("rocm_arch", "gfx1151"));
                        const auto paths = facts.value("runtime_paths", std::vector<std::string>{"/oracle/therock/core", "/oracle/therock/blas"});
                        std::string joined;
                        for (const auto& path : paths) { if (!joined.empty()) joined += host.os == "windows" ? ";" : ":"; joined += path; }
                        return joined;
                    }
                    case typed::RuntimeQuery::HomeDirectory: events.push_back("passwd-home"); return facts.value("home", "/oracle/passwd/home");
                }
                throw std::logic_error("Unknown runtime query");
            },
            [this](const auto& architecture, const auto& destination) { events.push_back("stage:" + architecture + ":" + destination); }
        };
    }
};
inline Json load_case(const Json& input, const std::string& view, const DefinitionRunner& runner, const BackendSpec& spec) {
    Sources sources(input, view);
    Json actual = Json::object();
    int device = static_cast<int>(DEVICE_NPU);
    try {
        RecipeOptions options("llamacpp", {{"ctx_size", 4096}, {"llamacpp_backend", view == "macos_arm64" ? "metal" : "cpu"}});
        options.set_option("llamacpp_device", ""); options.set_option("llamacpp_args", "");
        const auto option_values = input.value("options", Json::object());
        for (const auto& [key, value] : option_values.items()) options.set_option(key, value);
        const auto info = model(input.value("model", Json::object()));
        actual["requested_options"] = options.to_json();
        runner.resolve_runtime_options(info, options);
        actual["resolved_options"] = options.to_json();
        const auto prepared = runner.prepare_load({"fixture", info, options, input.value("level", "error"), {}, {},
            [&](DeviceType selected) { device = static_cast<int>(selected); }}, spec, sources, {}, sources.environment_sources());
        const auto& plan = prepared.plan;
        sources.events.push_back("start");
        actual["command"] = {{"program", plan.command.program}, {"argv", plan.command.args}, {"env", plan.command.env}, {"port", plan.command.port},
            {"ready_endpoint", plan.command.ready_endpoint}, {"cwd", plan.working_directory}, {"inherit_output", plan.output.inherit}, {"filter_health_logs", plan.output.filter_health_logs}};
        actual["options_after_load"] = options.to_json();
        actual["error"] = "";
    } catch (const std::invalid_argument& error) { actual["error"] = error.what(); actual["exception"] = "invalid_argument"; }
    catch (const std::exception& error) { actual["error"] = error.what(); actual["exception"] = "exception"; }
    actual["device"] = device;
    actual["events"] = sources.events;
    if (!actual.contains("command")) actual["command"] = nullptr;
    return normalize(actual);
}
template <typename T>
void write_number(std::ostream& output, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) output.put(static_cast<char>((value >> (8 * i)) & 255));
}

void write_string(std::ostream& output, const std::string& value) {
    write_number<std::uint64_t>(output, value.size());
    output.write(value.data(), value.size());
}

inline Json artifact_case(const Json& input, const DefinitionRunner& runner) {
    const auto temp_path = test::make_scratch_dir("llamacpp-v2-artifacts");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } temp{temp_path};
    const fs::path cache = temp.path / "cache";
    if (input.value("create_cache", true)) fs::create_directory(cache);
    const Json file_values = input.value("files", Json::object());
    for (const auto& [name, content] : file_values.items()) {
        const auto path = cache / name;
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (content.is_object()) {
            file.write("GGUF", 4);
            write_number<std::uint32_t>(file, 3);
            write_number<std::uint64_t>(file, 0);
            write_number<std::uint64_t>(file, content.size());
            for (const auto& [key, value] : content.items()) {
                write_string(file, key);
                write_number<std::uint32_t>(file, value.is_string() ? 8 : 4);
                if (value.is_string()) write_string(file, value.get<std::string>());
                else write_number<std::uint32_t>(file, value.get<std::uint32_t>());
            }
        } else file << content.get<std::string>();
    }
    Json result;
    const std::string variant = input.value("variant", "");
    result["resolved"] = [&] { CheckpointResolveContext ctx; ctx.model_cache_path = cache.string(); ctx.type = "main"; ctx.variant = variant; return runner.resolve_artifact(ctx); }();
    result["imported"] = runner.find_imported(cache.string());
    CheckpointResolveContext context;
    context.model_cache_path = cache.string();
    context.variant = variant;
    context.type = "main";
    auto info = model(input.value("model", Json::object()));
    result["checkpoint"] = runner.resolve_artifact(context);
    result["files"] = Json::object();
    for (const auto& [name, content] : file_values.items()) {
        (void)content;
        result["files"][name] = runner.validate_file((cache / name).string());
    }
    result["directory"] = runner.validate_file(cache.string());
    result["registration"] = runner.validate_registration(input.value("registration", "repo/model-GGUF"));
    info.resolved_paths["main"] = (cache / input.value("metadata_file", "missing.gguf")).string();
    runner.populate_metadata(info);
    result["metadata"] = {{"architecture", info.gguf.architecture}, {"context", info.max_context_window},
        {"vision", info.gguf.caps.vision}, {"tool_calling", info.gguf.caps.tool_calling}, {"mtp", info.gguf.caps.mtp}, {"labels", info.labels}};
    return normalize(result, temp.path.string());
}

inline void witnesses(const std::shared_ptr<const ExecutableDefinition>& executable, const std::shared_ptr<const ConfiguredBackendEntry>& entry) {
    for (const std::string backend : {"cpu", "vulkan", "invalid"}) {
        auto sources = std::make_shared<Sources>(Json{{"facts", {{"install_error", "injected install failure"}}}}, "linux_x64");
        ConfiguredServer server(entry, BackendContext{"error"}, ConfiguredServerServices{sources,
            [](auto, const auto&) -> std::unique_ptr<ServerProcess> { throw std::logic_error("No process on failed load"); }});
        auto info = model(Json::object());
        RecipeOptions options("llamacpp", {{"ctx_size", 4096}, {"llamacpp_backend", backend}});
        options.set_option("llamacpp_device", ""); options.set_option("llamacpp_args", "");
        server.set_model_metadata("fixture", info.checkpoint(), info.type, DEVICE_NPU, options);
        check(throws([&] { server.load("fixture", info, options); }) && server.get_device_type() ==
            (backend == "invalid" ? DEVICE_NPU : backend == "cpu" ? DEVICE_CPU : DEVICE_GPU),
            "ConfiguredServerAssignsDeviceBeforeInstallationFailure:" + backend);
    }
    DefinitionRunner runner(executable);
    auto launch = Launch(); launch.port = 0; launch.allow_install = true;
    auto info = model(Json::object());
    RecipeOptions options("llamacpp", {{"ctx_size", 4096}, {"llamacpp_backend", "cpu"}});
    options.set_option("llamacpp_device", ""); options.set_option("llamacpp_args", "");
    check(!throws([&] {
        const auto prepared = runner.prepare_load({"fixture", info, options, "error", {}, {}}, entry->spec, launch);
        if (prepared.plan.command.port != 0) throw std::logic_error("Port changed");
    }), "DeclaredZeroPortPolicyAllowsZero");
    auto document = Json::parse(read_bytes("llamacpp.candidate.json"));
    document["merge"]["launch"]["ports"][0]["reject_zero"] = true;
    const auto rejecting = runtime_fixture::executable(document);
    const auto rejecting_entry = ConfiguredBackendEntry::make(rejecting);
    check(throws([&] { DefinitionRunner(rejecting).prepare_load({"fixture", info, options, "error", {}, {}}, rejecting_entry->spec, launch); }, "Invalid configured port"),
        "DeclaredZeroPortPolicyRejectsZero");
}

inline void parity(const std::shared_ptr<const ExecutableDefinition>& executable, const ConfiguredBackendEntry& entry) {
    const DefinitionRunner runner(executable);
    const auto cases = read("cases");
    unsigned total = 0;
    for (const auto& view : std::array<std::string, 4>{"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
        const auto expected = read(view).at("cases");
        unsigned loads = 0, artifacts = 0;
        Json results = Json::object();
        for (const auto& input : cases) {
            const auto kind = input.at("kind");
            if (kind != "load" && kind != "gguf") continue;
            const auto id = input.at("id").get<std::string>();
            Json actual;
            if (kind == "load") { actual = load_case(input, view, runner, entry.spec); ++loads; }
            else {
                try { actual = {{"gguf", artifact_case(input, runner)}, {"error", ""}}; }
                catch (const std::invalid_argument& error) { actual = {{"error", error.what()}, {"exception", "invalid_argument"}}; }
                catch (const std::exception& error) { actual = {{"error", error.what()}, {"exception", "exception"}}; }
                actual["events"] = Json::array(); actual["command"] = nullptr; ++artifacts;
            }
            results[id] = actual;
            const auto difference = Json::diff(expected.at(id), actual);
            check(difference.empty(), "Frozen" + std::string(kind == "load" ? "Load" : "Artifact") + "Parity:" + view + ":" + id);
            if (!difference.empty()) std::cout << difference.dump() << '\n';
            else ++total;
        }
        check(loads == 321 && artifacts == 30, "FrozenLoadArtifactCaseCounts:" + view);
        if (const auto* directory = std::getenv("LEMON_CORPUS_OUT")) std::ofstream(fs::path(directory) / (view + ".json")) << results.dump(2) << '\n';
    }
    check(total == 1404, "FrozenLoadArtifactAll1404Cases");
}
}
