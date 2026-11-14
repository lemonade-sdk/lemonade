#pragma once

namespace {
struct C7bLog {
    explicit C7bLog(std::vector<std::string>& lines) {
        AixLog::Log::init<AixLog::SinkCallback>(AixLog::Severity::trace, [&lines](const AixLog::Metadata& metadata, const std::string& message) {
            if (metadata.tag.text == "RocmFpx" || metadata.tag.text == "LlamaCpp" || metadata.tag.text == "Halogen" || metadata.tag.text == "DS4")
                lines.push_back(AixLog::to_string(metadata.severity) + " " + metadata.tag.text + " " + message);
        });
    }
    ~C7bLog() { AixLog::Log::init({}); }
};
struct C7bServices : LogServices {
    int port = 1;
    explicit C7bServices(std::vector<std::string>& events) : LogServices(events) {}
    int allocate_port() override { LogServices::allocate_port(); return port; }
};
class C7bClient final : public ServerProcess {
public:
    bool alive = false;
    C7bClient() : ServerProcess({}) {}
    std::string start(const ServerCommand& command) override {
        check(command.program == "podman", "C7bOnlyFakeContainerClient");
        alive = true; return "127.0.0.1";
    }
    void stop() override { alive = false; }
    bool running() const override { return alive; }
};
utils::ContainerHost c7b_host(std::vector<std::string>& events) {
    utils::ContainerHost host;
    host.on_path = [](const auto& name) { return name == "podman" || name == "getsubids"; };
    host.gpu_devices_accessible = [] { return true; };
    host.group_id = [](const auto&) { return "109"; };
    host.account = [] { return "fixture"; };
    host.env = [](const auto&) { return ""; };
    host.is_root = [] { return false; };
    host.read_file = [](const auto&) { return ""; };
    host.run = [&](const auto& argv, int, bool) {
        events.push_back("fake-host:" + argv.at(0));
        if (argv[0] == "getsubids") return utils::CommandResult{0, "0: fixture 100000 65536"};
        if (argv[1] == "info") return utils::CommandResult{0, "true netavark"};
        return utils::CommandResult{0, ""};
    };
    host.stream = [](const auto&, int, const auto&) -> int { throw std::logic_error("Pull forbidden"); };
    host.canonical = [](const auto& path) { return std::filesystem::canonical(path).string(); };
    host.file_kind = [](const auto& path) { return std::filesystem::is_directory(path) ? utils::ContainerFileKind::Directory : utils::ContainerFileKind::File; };
    host.files = [](const auto&) { return std::vector<std::string>{}; };
    host.sleep = [](auto) {};
    return host;
}
std::string c7b_replace(std::string text, const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) { text.replace(at, from.size(), to); at += to.size(); }
    return text;
}
void c7b_evidence(const std::string& name, const Json& value) {
    const auto* root = std::getenv("LEMONADE_C7B_EVIDENCE");
    if (!root) return;
    std::filesystem::create_directories(root);
    std::ofstream(std::filesystem::path(root) / (name + ".json")) << value.dump(2) << '\n';
}
std::vector<std::string> c7b_expected(const Json& fixture, const std::string& main, int port) {
    std::vector<std::string> lines;
    for (const auto& row : fixture.at("expected")) {
        const auto severity = row.at("level") == "info" ? "Info" : "Debug";
        auto text = c7b_replace(row.at("line"), "@MAIN@", main);
        text = c7b_replace(text, "@PORT@", std::to_string(port));
        std::istringstream input(text);
        std::string line;
        while (std::getline(input, line)) lines.push_back(std::string(severity) + " " + row.at("category").get<std::string>() + " " + line);
    }
    return lines;
}
bool c7b_compile_rejects(execution::Program program, const Json& records, bool erase_legacy = true) {
    if (erase_legacy) program.normalized["launch"].erase("builtin_load_log");
    program.normalized["launch"]["diagnostics"] = records;
    program.launch.diagnostics.clear();
    execution::Budget budget{ExecutionCompileLimits{}, {}};
    try { execution::compile_diagnostics(program, budget); }
    catch (const execution::Failure&) { return true; }
    return false;
}
void c7b_diagnostic_boundaries() {
    auto doc = Json::parse(read_bytes("c7b.halogen.json")).at("definition");
    doc["merge"]["launch"].erase("direct_stream_policy");
    const auto value = candidate(doc);
    const auto& program = execution::Inspection::program(*value.compiled);
    const auto records = doc.at("merge").at("launch").at("diagnostics");
    auto external = root("c7b-external-diagnostics");
    external["merge"]["metadata"]["log_category"] = "Halogen";
    external["merge"]["launch"]["diagnostics"] = records;
    const auto admitted = admit({external}, empty_builtins());
    check(admitted.catalog && !compile_execution_definition(admitted.catalog->v2_definition("c7b-external-diagnostics")).definition,
        "C7bExternalDiagnosticsRejected");
    auto legacy_external = external;
    legacy_external["merge"]["launch"].erase("diagnostics");
    legacy_external["merge"]["launch"]["builtin_load_log"] = "halogen_starting_v1";
    const auto legacy_admitted = admit({legacy_external}, empty_builtins());
    check(legacy_admitted.catalog && !compile_execution_definition(legacy_admitted.catalog->v2_definition("c7b-external-diagnostics")).definition,
        "C7aExternalLoadLogRejected");
    check(!candidate(doc, false).compiled, "C7bUngrantedDiagnosticsRejected");
    auto local = root("c7b-local-diagnostics");
    local["merge"]["metadata"]["log_category"] = "Halogen";
    local["merge"]["launch"]["diagnostics"] = records;
    const auto local_admitted = admit({}, builtins(false, &local));
    check(local_admitted.catalog && !compile_execution_definition(local_admitted.catalog->builtin_v2_definition(
        {DefinitionNamespace::Builtin, "c7b-local-diagnostics", 1})).definition, "C7bLocalBuiltinDiagnosticsRejected");
    auto declaration = value.compiled->normalized_declaration();
    for (const auto* field : {"id", "phase", "level", "category", "format", "when"}) {
        auto changed = declaration;
        changed["launch"]["diagnostics"][0][field] = "changed";
        check(!AuthorityInspection::permits_execution(*value.token, changed), "C7bDiagnosticFieldSealed:" + std::string(field));
    }
    auto changed = declaration;
    changed["launch"].erase("diagnostics");
    check(!AuthorityInspection::permits_execution(*value.token, changed), "C7bDiagnosticRemovalSealed");
    changed = declaration; changed["metadata"]["log_category"] = "DS4";
    check(!AuthorityInspection::permits_execution(*value.token, changed), "C7bDiagnosticCategorySealed");
    changed = declaration; std::reverse(changed["launch"]["prepare_order"].begin(), changed["launch"]["prepare_order"].end());
    check(!AuthorityInspection::permits_execution(*value.token, changed), "C7bDiagnosticPreparationSealed");
    changed = declaration; changed["launch"]["ports"][0]["reject_zero"] = false;
    check(!AuthorityInspection::permits_execution(*value.token, changed), "C7bDiagnosticPortProducerSealed");
    auto broken = records; broken[0]["format"] = "arbitrary literal";
    check(c7b_compile_rejects(program, broken), "C7bArbitraryFormatRejected");
    broken = records; broken[0]["format"] = "Starting {environment:string} on port {port:integer}";
    check(c7b_compile_rejects(program, broken), "C7bUnboundedFactFormatRejected");
    bool rejected = false;
    try { execution::diagnostic_parts("{environment:string}"); } catch (const execution::Failure&) { rejected = true; }
    check(rejected, "C7bFactOutsideClosedSetRejected");
    broken = records; broken[0]["phase"] = "loading";
    check(c7b_compile_rejects(program, broken), "C7bFactBeforeItsPhaseRejected");
    broken = records; broken[0]["phase"] = "unbounded";
    check(c7b_compile_rejects(program, broken), "C7bUnknownPhaseRejected");
    broken = records; broken[0]["when"] = "arbitrary";
    check(c7b_compile_rejects(program, broken), "C7bUnknownConditionRejected");
    broken = records; broken[0]["level"] = "arbitrary";
    check(c7b_compile_rejects(program, broken), "C7bUnknownLevelRejected");
    broken = records; broken[0]["category"] = "Other";
    check(c7b_compile_rejects(program, broken), "C7bUnknownCategoryRejected");
    auto unknown_category = program; unknown_category.normalized["metadata"]["log_category"] = "Other";
    check(c7b_compile_rejects(unknown_category, broken), "C7bCategoryDomainClosed");
    broken = records; broken.push_back(records[0]);
    check(c7b_compile_rejects(program, broken), "C7bDuplicateDiagnosticRejected");
    broken = Json::array(); for (unsigned i = 0; i < 33; ++i) { auto row = records[0]; row["id"] = "record" + std::to_string(i); broken.push_back(row); }
    check(c7b_compile_rejects(program, broken), "C7bRecordCountBounded");
    broken = records; broken[0]["id"] = std::string(65, 'x');
    check(c7b_compile_rejects(program, broken), "C7bDiagnosticIdBounded");
    broken = records; auto early = records[0]; early["id"] = "early"; early["phase"] = "loading"; early["format"] = "Loading model: {model:string}"; broken.push_back(early);
    check(c7b_compile_rejects(program, broken), "C7bDiagnosticOrderRejected");
    auto mixed = program; mixed.normalized["launch"]["load_log_policy"] = "loading_and_options";
    check(c7b_compile_rejects(mixed, records), "C7bMixedGenericLogsRejected");
    mixed = program; mixed.normalized["launch"]["builtin_load_log"] = "halogen_starting_v1";
    broken = records; broken[0]["id"] = "data-starting";
    check(c7b_compile_rejects(mixed, broken, false), "C7bMixedBindingsRejected");
    bool keyed = false;
    try {
        DefinitionLimits limits; std::size_t retained = 0;
        v2::Budget budget{limits, {}, retained}; v2::LayerState state;
        const DefinitionKey key{DefinitionNamespace::Builtin, "c7b-keyed", 1};
        v2::apply_layer(state, Json{{"merge", {{"launch", {{"diagnostics", records}}}}}}, key, budget);
        auto added = records[0]; added["id"] = "second";
        v2::apply_layer(state, Json{{"edits", Json::array({{{"op", "add"}, {"path", "/launch/diagnostics"},
            {"after", {{"id", "starting"}}}, {"value", added}}})}}, key, budget);
        keyed = v2::materialize(state.root, budget).at("launch").at("diagnostics") == Json::array({records[0], added});
    } catch (const v2::EditFailure&) {}
    check(keyed, "C7bDiagnosticsAreKeyedRecords");
    const auto llama = candidate(Json::parse(read_bytes("c7b.halobox.json")).at("definition"));
    changed = llama.compiled->normalized_declaration(); std::swap(changed["launch"]["diagnostics"][0], changed["launch"]["diagnostics"][1]);
    check(!AuthorityInspection::permits_execution(*llama.token, changed), "C7bWithinPhaseOrderSealed");
    changed = llama.compiled->normalized_declaration(); changed["launch"]["custom_args"]["option"] = "secret";
    check(!AuthorityInspection::permits_execution(*llama.token, changed), "C7bDiagnosticOptionProducerSealed");
    external = root("c7b-external-rewrite");
    external["merge"]["operations"][0]["request"]["content_transform"] = "ds4_no_think_v1";
    external["merge"]["operations"][0]["request"]["rewrite_domain"] = "canonical_path";
    const auto rewrite_admitted = admit({external}, empty_builtins());
    check(rewrite_admitted.catalog && !compile_execution_definition(rewrite_admitted.catalog->v2_definition("c7b-external-rewrite")).definition,
        "C7bExternalRewriteDomainRejected");
}
void c7b_rewrite(const Json& fixture) {
    const auto value = candidate(fixture.at("definition"));
    const auto bound = executable(value);
    DefinitionRunner runner(bound);
    ExecutionSession session{bound};
    const bool substring = fixture.at("rewrite_domain") == "chat_completions_substring";
    const bool ds4 = fixture.at("engine") == "ds4";
    const std::string body = "{ \"messages\": [{\"role\":\"user\",\"content\":\"/no_think\\nquestion\"}]}";
    httplib::Server http;
    std::string wire_path, wire_body;
    http.Post("/.*", [&](const auto& request, auto& response) { wire_path = request.target; wire_body = request.body; response.set_content("ok", "text/event-stream"); });
    const auto port = http.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("C7b HTTP bind");
    std::thread listener([&] { http.listen_after_bind(); });
    struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); thread.join(); } } cleanup{http, listener};
    http.wait_until_ready();
    for (const auto* path : {"/v1/chat/completions", "/v1/chat/completions?probe=1", "/other/chat/completions-suffix?x=1", "/arbitrary?x=chat/completions", "/v1/responses?probe=1", "/arbitrary/unrelated"}) {
        for (const auto& input : {body, std::string("{broken")}) {
            std::string expected = input;
            const auto parsed = Json::parse(input, nullptr, false);
            if (!parsed.is_discarded() && ds4 && (std::string(path) == "/v1/chat/completions" ||
                (substring && std::string(path).find("chat/completions") != std::string::npos))) {
                auto source = parsed;
                source["messages"][0]["content"] = "question";
                expected = source.dump();
            } else if (!parsed.is_discarded() && !ds4 && fixture.at("engine") != "halogen" &&
                (std::string(path) == "/v1/chat/completions" || std::string(path) == "/v1/responses")) {
                auto source = parsed;
                if (std::string(path) == "/v1/chat/completions") utils::JsonUtils::add_legacy_max_tokens_alias(source);
                expected = backends::llamacpp::sanitize_tool_schema_limits(std::move(source)).dump();
            }
            const auto mapped = runner.map_stream(path, ModelType::LLM, session, input, false, 37);
            httplib::Client client("127.0.0.1", port); client.set_connection_timeout(2, 0); client.set_read_timeout(2, 0);
            const auto response = client.Post(mapped.path, mapped.original_body.value_or(mapped.body.dump()), "application/json");
            check(response && response->body == "ok" && wire_path == path && wire_body == expected && mapped.timeout_seconds == 37 && !mapped.sse,
                "C7bExactRewriteWire:" + fixture.at("engine").get<std::string>() + path + input);
        }
    }
    if (substring) {
        auto declaration = value.compiled->normalized_declaration();
        declaration["operations"][0]["request"]["rewrite_domain"] = "canonical_path";
        check(!AuthorityInspection::permits_execution(*value.token, declaration), "C7bRewriteDomainSealed");
        auto strict_doc = fixture.at("definition");
        strict_doc["merge"]["launch"]["direct_stream_policy"] = "strict";
        strict_doc["merge"]["operations"][0]["request"]["rewrite_domain"] = "canonical_path";
        const auto strict = candidate(strict_doc);
        declaration = strict.compiled->normalized_declaration();
        declaration["operations"][0]["request"]["rewrite_domain"] = "chat_completions_substring";
        check(!AuthorityInspection::permits_execution(*strict.token, declaration), "C7bRewriteDomainSealed");
        for (const auto& path : {std::string("//foreign/chat/completions"), std::string("/chat/completions\r\nHeader"), std::string("/chat/completions\0x", 19)})
            check(refuses([&] { runner.map_stream(path, ModelType::LLM, session, body, true, 1); }), "C7bRewriteEndpointConfinement");
        auto doc = fixture.at("definition");
        for (auto& op : doc["merge"]["operations"]) if (op["operation"] == "chat_completion") op["request"]["rewrite_domain"] = "arbitrary";
        check(refuses([&] { candidate(doc); }), "C7bArbitraryRewriteDomainRejected");
        doc = fixture.at("definition");
        doc["merge"]["operations"][0]["request"]["hooks"] = Json::array({{{"id", "llamacpp.max_tokens_alias"}, {"params", Json::object()}}});
        check(!candidate(doc).compiled, "C7bRewriteRejectsAdditionalRequestHooks");
        doc = fixture.at("definition");
        doc["merge"]["operations"][0]["model_modes"] = Json::array({"chat"});
        auto other = doc["merge"]["operations"][0]; other["id"] = "other-model-cell";
        other["model_modes"] = Json::array({"embedding"}); other["request"].erase("rewrite_domain");
        doc["merge"]["operations"].push_back(other);
        const auto multi = executable(candidate(doc));
        DefinitionRunner multi_runner(multi); ExecutionSession multi_session{multi};
        const auto mapped = multi_runner.map_stream("/v1/chat/completions?cell=1", ModelType::EMBEDDING, multi_session, body, true, 1);
        check(mapped.original_body == body, "C7bRewriteDomainBoundToModelCell");
        auto canonical = fixture;
        canonical["rewrite_domain"] = "canonical_path";
        for (auto& op : canonical["definition"]["merge"]["operations"]) op["request"].erase("rewrite_domain");
        c7b_rewrite(canonical);
    }
}
void c7b_engine(const std::string& name) {
    namespace fs = std::filesystem;
    const auto fixture = Json::parse(read_bytes("c7b." + name + ".json"));
    const auto doc = fixture.at("definition");
    const auto backend = ConfiguredBackendEntry::make(executable(candidate(doc)));
    const auto scratch = fs::path(LEMON_C7B_SCRATCH) / name;
    fs::create_directories(scratch / "tokenizer");
    const auto main = (scratch / (name == "halogen" ? "main.hgn" : "main.fake")).string();
    std::ofstream(main) << "fake"; std::ofstream(scratch / "table.hgn") << "fake"; std::ofstream(scratch / "tokenizer/tokenizer.json") << "fake";
    ModelInfo model; model.recipe = "gufo"; model.type = ModelType::LLM; model.resolved_paths["main"] = main; model.extras["halogen_lookup_table"] = "table.hgn";
    RecipeOptions options("gufo", Json::object()); options.set_option("gufo_backend", ""); options.set_option("ctx_size", 8192); options.set_option("gufo_args", "--fixture 7");
    httplib::Server http;
    http.Get("/.*", [](const auto&, auto& response) { response.set_content("{}", "application/json"); });
    const auto port = http.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw std::runtime_error("C7b readiness bind");
    std::thread listener([&] { http.listen_after_bind(); });
    struct Cleanup { httplib::Server& server; std::thread& thread; ~Cleanup() { server.stop(); thread.join(); } } cleanup{http, listener};
    http.wait_until_ready();
    std::vector<std::string> events, lines;
    C7bLog capture(lines);
    auto services = std::make_shared<C7bServices>(events); services->port = port;
    ConfiguredServerServices effects; effects.launch = services;
    effects.containers = std::make_shared<utils::ContainerManager>(c7b_host(events));
    bool fail_create = false;
    effects.make_container_client = [&](auto) -> std::unique_ptr<ServerProcess> { events.push_back("create"); if (fail_create) throw std::runtime_error("fake failure"); return std::make_unique<C7bClient>(); };
    effects.make_process = [](auto, const auto&) -> std::unique_ptr<ServerProcess> { throw std::logic_error("Native forbidden"); };
    ConfiguredServer server(backend, {"trace", nullptr, nullptr}, effects);
    server.load("fixture", model, options);
    auto expected = c7b_expected(fixture, main, port);
    for (auto& line : expected) {
        const std::string prefix = "Debug LlamaCpp Per-model settings: ";
        if (line.rfind(prefix, 0) == 0) line = prefix + "ctx_size=8192, gufo_args=--fixture 7, merge_args=true, auto_evict=(none), evict_idle_timeout=300, downsize_idle_timeout=60, evict_weight_factor=1.000000, pinned=false, auto_update=(none)";
    }
    check(lines == expected, "C7bExactOrderedDiagnostics:" + name);
    check(std::count(events.begin(), events.end(), "install") == 1, "C7bContainerImageInstalledOnce:" + name);
    c7b_evidence(name, {{"actual", lines}, {"expected", expected}, {"events", events}, {"port", port}});
    if (lines != expected) for (const auto& line : lines) std::printf("observed: %s\n", line.c_str());
    server.unload();
    lines.clear(); events.clear(); fail_create = true;
    check(refuses([&] { server.load("fixture", model, options); }), "C7bFakeFailureObserved:" + name);
    auto failure_expected = expected;
    if (name == "rocmfpx" || name == "halobox" || name == "nathanw") failure_expected.pop_back();
    check(lines == failure_expected, "C7bNoSuccessAfterFailure:" + name);
    lines.clear(); events.clear(); services->fail_install = true;
    check(refuses([&] { server.load("fixture", model, options); }), "C7bImageInstallFailureObserved:" + name);
    std::vector<std::string> install_expected;
    if (name == "rocmfpx") install_expected.assign(expected.begin(), expected.begin() + 1);
    if (name == "halobox" || name == "nathanw") install_expected.assign(expected.begin(), expected.begin() + 3);
    check(lines == install_expected, "C7bInstallFailureDiagnosticAdmission:" + name);
    lines.clear(); events.clear(); services->fail_install = false; services->fail_port = true;
    check(refuses([&] { server.load("fixture", model, options); }), "C7bPortFailureObserved:" + name);
    auto port_expected = install_expected;
    if (name == "halobox" || name == "nathanw") port_expected.assign(expected.begin(), expected.begin() + 4);
    check(lines == port_expected, "C7bPortFailureDiagnosticAdmission:" + name);
    services->fail_port = false;
    lines.clear(); events.clear();
    check(refuses([&] { server.load("fixture", model, options); }), "C7bRepeatedProcessFailureObserved:" + name);
    if (name == "halogen") {
        const auto data_events = events; const auto data_lines = lines;
        const auto legacy = ConfiguredBackendEntry::make(executable(candidate(Json::parse(read_bytes("c7a.log.json")).at("definition"))));
        ConfiguredServer old(legacy, {"trace", nullptr, nullptr}, effects);
        lines.clear(); events.clear();
        check(refuses([&] { old.load("fixture", model, options); }) && lines == data_lines && events == data_events,
            "C7bHalogenC7aByteAndOrderIdentity");
        c7b_evidence("halogen-c7a-comparison", {{"c7a_lines", lines}, {"data_lines", data_lines},
            {"c7a_events", events}, {"data_events", data_events}});
        lines.clear(); events.clear();
        check(refuses([&] { server.load(std::string(16385, 'x'), model, options); }) && lines.empty() &&
            std::find(events.begin(), events.end(), "create") == events.end(), "C7bDiagnosticFactByteBound");
        auto failure_doc = doc;
        auto row = failure_doc["merge"]["launch"]["diagnostics"][0];
        row["id"] = "failure"; row["phase"] = "failure"; row["level"] = "error";
        failure_doc["merge"]["launch"]["diagnostics"].push_back(row);
        const auto failure_backend = ConfiguredBackendEntry::make(executable(candidate(failure_doc)));
        ConfiguredServer failure_server(failure_backend, {"trace", nullptr, nullptr}, effects);
        lines.clear(); events.clear();
        check(refuses([&] { failure_server.load("fixture", model, options); }) &&
            lines == std::vector<std::string>{"Info Halogen Starting fixture on port " + std::to_string(port),
                "Error Halogen Starting fixture on port " + std::to_string(port)}, "C7bFailurePhaseAfterStarting");
        lines.clear(); events.clear(); services->fail_port = true;
        check(refuses([&] { failure_server.load("fixture", model, options); }) && lines.empty(), "C7bFailurePhaseExcludesPreparationFailures");
        services->fail_port = false;
        auto warning_doc = doc;
        warning_doc["merge"]["launch"]["diagnostics"][0]["level"] = "warning";
        const auto warning_backend = ConfiguredBackendEntry::make(executable(candidate(warning_doc)));
        ConfiguredServer warning_server(warning_backend, {"trace", nullptr, nullptr}, effects);
        lines.clear(); events.clear();
        check(refuses([&] { warning_server.load("fixture", model, options); }) &&
            lines == std::vector<std::string>{"Warn Halogen Starting fixture on port " + std::to_string(port)}, "C7bWarningSeverity");
    }
    if (name == "halobox" || name == "nathanw") {
        fail_create = false;
        for (const auto mode : {ModelType::EMBEDDING, ModelType::RERANKING}) {
            model.type = mode; lines.clear(); events.clear();
            server.load("fixture", model, options);
            auto mode_expected = expected;
            mode_expected.insert(mode_expected.end() - 3, mode == ModelType::EMBEDDING ?
                "Info LlamaCpp Model supports embeddings, adding --embeddings flag" :
                "Info LlamaCpp Model supports reranking, adding --reranking flag");
            check(lines == mode_expected, "C7bConditionalModelDiagnostics:" + name);
            server.unload();
        }
        model.type = ModelType::LLM;
        options.set_option("gufo_args", ""); lines.clear(); events.clear();
        server.load("fixture", model, options);
        auto empty_expected = expected; empty_expected.erase(empty_expected.end() - 3);
        for (auto& line : empty_expected) line = c7b_replace(line, "gufo_args=--fixture 7", "gufo_args=(none)");
        check(lines == empty_expected, "C7bEmptyCustomDiagnosticsSuppressed:" + name);
        server.unload();
    }
    if (name == "ds4") {
        const auto& log = backend->launch_compatibility.load_log;
        lines.clear();
        const Json facts{{"diagnostic_facts", {{"image", std::string(16384, 'i')}, {"main", std::string(16384, 'm')}}}};
        check(log && log->starting && refuses([&] { log->starting("DS4", facts, ServerCommand{}); }) && lines.empty(), "C7bDiagnosticLineByteBound");
    }
    c7b_rewrite(fixture);
}
void c7b_contract() {
    c7b_diagnostic_boundaries();
    for (const auto* name : {"rocmfpx", "halobox", "nathanw", "halogen", "ds4"}) c7b_engine(name);
}
}
