#pragma once

namespace {
struct LogServices final : LaunchServices {
    std::vector<std::string>& events;
    bool fail_install = false, fail_port = false;
    explicit LogServices(std::vector<std::string>& value) : events(value) {}
    bool exists(const std::string& path) const override { return std::filesystem::exists(path); }
    std::vector<std::string> supported_backends(const std::string&) const override { return {}; }
    void validate_backend(const std::string&, const std::string&) const override {}
    std::string external_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("native forbidden"); }
    void install_backend(const std::string&, const std::string&) override { throw std::logic_error("native forbidden"); }
    std::string managed_binary(const BackendSpec&, const std::string&) const override { throw std::logic_error("native forbidden"); }
    int allocate_port() override { events.push_back("port"); if (fail_port) throw std::runtime_error("port failure"); return 1; }
    int find_free_port(int) override { return 1; }
    std::vector<std::pair<std::string, std::string>> environment(const std::string&, const std::string&, const std::string&, const std::string&) const override { return {}; }
    PlatformInfo platform() const override { return {"linux", "x86_64", "gfx1151"}; }
    long default_timeout_seconds() const override { return 2; }
    std::string container_image(const ContainerPolicy& policy) const override { return policy.repository + "@sha256:" + std::string(64, 'a'); }
    void install_container_image(const std::string&) override { events.push_back("install"); if (fail_install) throw std::runtime_error("install failure"); }
    std::vector<ContainerGpuNode> container_gpu_nodes() const override { return {{2, "gfx1151"}}; }
    std::string container_gpu_properties(unsigned) const override { return "capability 134217728\nctl_stack_size 1024\nsimd_count 5\ncwsr_size 959488\n"; }
    bool container_supports_arch(const std::string&, const std::string&, const std::string&) const override { return true; }
};
struct CapturedLog {
    explicit CapturedLog(std::vector<std::string>& records) {
        AixLog::Log::init<AixLog::SinkCallback>(AixLog::Severity::trace, [&records](const AixLog::Metadata& metadata, const std::string& message) {
            if (metadata.tag.text == "Halogen") records.push_back(AixLog::to_string(metadata.severity) + " " + message);
        });
    }
    ~CapturedLog() { AixLog::Log::init({}); }
};
void load_log_contract() {
    namespace fs = std::filesystem;
    const auto scratch = fs::path(LEMON_C7A_SCRATCH) / "log";
    fs::create_directories(scratch / "tokenizer");
    std::ofstream(scratch / "main.hgn") << "fake";
    std::ofstream(scratch / "tokenizer/tokenizer.json") << "fake";
    std::ofstream(scratch / "table.hgn") << "fake";
    const auto fixture = Json::parse(read_bytes("c7a.log.json"));
    auto doc = fixture.at("definition");
    const auto value = candidate(doc);
    const auto backend = ConfiguredBackendEntry::make(executable(value));
    check(backend->launch_compatibility.load_log && backend->launch_compatibility.load_log->starting &&
        !backend->launch_compatibility.load_log->loading && !backend->launch_compatibility.load_log->prepared, "C7aV2BuiltinLoadLogBound");
    auto declaration = value.compiled->normalized_declaration();
    declaration["launch"].erase("builtin_load_log");
    check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7aLoadLogBindingSealed");
    declaration = value.compiled->normalized_declaration(); declaration["metadata"]["log_category"] = "Other";
    check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7aLoadLogCategorySealed");
    check(!candidate(doc, false).compiled, "C7aUngrantedLoadLogRejected");
    auto external = root("c7a-log-external");
    external["merge"]["launch"]["builtin_load_log"] = "halogen_starting_v1";
    const auto admitted = admit({external}, empty_builtins());
    check(admitted.catalog && !compile_execution_definition(admitted.catalog->v2_definition("c7a-log-external")).definition, "C7aExternalLoadLogRejected");
    ModelInfo model; model.recipe = "gufo"; model.type = ModelType::LLM;
    model.resolved_paths["main"] = (scratch / "main.hgn").string(); model.extras["halogen_lookup_table"] = "table.hgn";
    RecipeOptions options("gufo", Json::object()); options.set_option("gufo_backend", ""); options.set_option("ctx_size", 8192);
    std::vector<std::string> events;
    CapturedLog capture(events);
    auto services = std::make_shared<LogServices>(events);
    ConfiguredServerServices effects;
    effects.launch = services;
    effects.make_container_client = [&](ProcessOutput) -> std::unique_ptr<ServerProcess> {
        events.push_back("create"); throw std::runtime_error("C7a fake process failure");
    };
    ConfiguredServer server(backend, {"trace", nullptr, nullptr}, effects);
    check(refuses([&] { server.load("fixture", model, options); }), "C7aFakeProcessFailureObserved");
    check(events == std::vector<std::string>{"port", "install", "Info Starting fixture on port 1", "create"}, "C7aStartingBeforeProcessFailureExactOrder");
    events.clear(); services->fail_port = true;
    check(refuses([&] { server.load("fixture", model, options); }) && events == std::vector<std::string>{"port"}, "C7aNoStartingBeforePortAdmission");
    events.clear(); services->fail_port = false; services->fail_install = true;
    check(refuses([&] { server.load("fixture", model, options); }) && events == std::vector<std::string>{"port", "install"}, "C7aNoStartingOnInstallFailure");
    events.clear(); services->fail_install = false;
    model.resolved_paths["main"] = (scratch / "absent.hgn").string();
    check(refuses([&] { server.load("fixture", model, options); }) && events.empty(), "C7aNoStartingBeforeArtifactAdmission");
    auto unselected = doc; unselected["merge"]["launch"].erase("builtin_load_log");
    check(!ConfiguredBackendEntry::make(executable(candidate(unselected)))->launch_compatibility.load_log, "C7aUnselectedLoadLogSilent");
    doc["merge"]["launch"]["builtin_load_log"] = "arbitrary";
    check(refuses([&] { candidate(doc); }), "C7aUnknownLoadLogRejected");
}
}
